#include "viewer/platform/wic_decoder.h"

#include <Windows.h>
#include <objbase.h>
#include <objidl.h>
#include <shobjidl_core.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace viewer::platform {
namespace {

using Microsoft::WRL::ComPtr;

template <typename T>
core::Result<T> failure(core::ErrorCode code, const wchar_t* message) {
  return core::Result<T>::failure({code, message});
}

class ComApartment {
 public:
  ComApartment() noexcept
      : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)),
        owns_initialization_(result_ == S_OK || result_ == S_FALSE) {}

  ComApartment(const ComApartment&) = delete;
  ComApartment& operator=(const ComApartment&) = delete;

  ~ComApartment() {
    if (owns_initialization_) {
      CoUninitialize();
    }
  }

  [[nodiscard]] bool usable() const noexcept {
    return owns_initialization_ || result_ == RPC_E_CHANGED_MODE;
  }

 private:
  HRESULT result_;
  bool owns_initialization_;
};

HRESULT create_factory(ComPtr<IWICImagingFactory>& factory) {
  HRESULT result =
      CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                       IID_PPV_ARGS(factory.GetAddressOf()));
  if (SUCCEEDED(result)) {
    return result;
  }
  if (!detail::should_fallback_to_wic1(result)) {
    return result;
  }

  factory.Reset();
  return CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                          CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(factory.GetAddressOf()));
}

core::Result<core::ImageFrame> decode_source_frame(
    IWICImagingFactory& factory,
    IWICBitmapSource& source,
    std::size_t byte_budget) {
  UINT width = 0;
  UINT height = 0;
  HRESULT result = source.GetSize(&width, &height);
  if (FAILED(result)) {
    return failure<core::ImageFrame>(
        core::ErrorCode::decode_error,
        L"WIC could not read image dimensions.");
  }

  auto frame = core::ImageFrame::allocate_bgra8(width, height, byte_budget);
  if (!frame.has_value()) {
    return frame;
  }

  ComPtr<IWICFormatConverter> converter;
  result = factory.CreateFormatConverter(converter.GetAddressOf());
  if (FAILED(result)) {
    return failure<core::ImageFrame>(
        core::ErrorCode::platform_error,
        L"WIC format converter creation failed.");
  }

  result = converter->Initialize(
      &source, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
      nullptr, 0.0, WICBitmapPaletteTypeCustom);
  if (FAILED(result)) {
    return failure<core::ImageFrame>(
        core::ErrorCode::decode_error,
        L"WIC pixel conversion failed.");
  }

  auto& output = frame.value();
  const UINT buffer_size = output.stride * output.height;
  result = converter->CopyPixels(
      nullptr, output.stride, buffer_size,
      reinterpret_cast<BYTE*>(output.pixels.data()));
  if (FAILED(result)) {
    return failure<core::ImageFrame>(
        core::ErrorCode::decode_error,
        L"WIC pixel copy failed.");
  }

  return core::Result<core::ImageFrame>::success(std::move(output));
}

core::Result<core::ImageFrame> decode_scaled_source_frame(
    IWICImagingFactory& factory,
    IWICBitmapSource& source,
    std::uint32_t max_edge,
    std::size_t byte_budget) {
  UINT width = 0;
  UINT height = 0;
  HRESULT result = source.GetSize(&width, &height);
  if (FAILED(result)) {
    return failure<core::ImageFrame>(
        core::ErrorCode::decode_error,
        L"WIC could not read thumbnail dimensions.");
  }

  if (max_edge == 0 || width == 0 || height == 0) {
    return failure<core::ImageFrame>(
        core::ErrorCode::invalid_format,
        L"Thumbnail dimensions must be non-zero.");
  }

  UINT target_width = width;
  UINT target_height = height;
  const UINT longest_edge = (std::max)(width, height);
  if (longest_edge > max_edge) {
    if (width >= height) {
      target_width = max_edge;
      target_height = (std::max)(
          1U, static_cast<UINT>(
                  (static_cast<std::uint64_t>(height) * max_edge) / width));
    } else {
      target_height = max_edge;
      target_width = (std::max)(
          1U, static_cast<UINT>(
                  (static_cast<std::uint64_t>(width) * max_edge) / height));
    }
  }

  ComPtr<IWICBitmapScaler> scaler;
  result = factory.CreateBitmapScaler(scaler.GetAddressOf());
  if (FAILED(result)) {
    return failure<core::ImageFrame>(
        core::ErrorCode::platform_error,
        L"WIC bitmap scaler creation failed.");
  }

  result = scaler->Initialize(&source, target_width, target_height,
                              WICBitmapInterpolationModeFant);
  if (FAILED(result)) {
    return failure<core::ImageFrame>(
        core::ErrorCode::decode_error,
        L"WIC thumbnail scaling failed.");
  }

  return decode_source_frame(factory, *scaler.Get(), byte_budget);
}

core::Result<core::ImageFrame> hbitmap_to_frame(HBITMAP bitmap,
                                                std::size_t byte_budget) {
  BITMAP description{};
  if (GetObjectW(bitmap, sizeof(description), &description) == 0 ||
      description.bmWidth <= 0 || description.bmHeight <= 0) {
    return failure<core::ImageFrame>(
        core::ErrorCode::decode_error,
        L"Shell thumbnail bitmap dimensions were invalid.");
  }

  auto frame = core::ImageFrame::allocate_bgra8(
      static_cast<std::uint32_t>(description.bmWidth),
      static_cast<std::uint32_t>(description.bmHeight), byte_budget);
  if (!frame.has_value()) {
    return frame;
  }

  auto& output = frame.value();
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = static_cast<LONG>(output.width);
  info.bmiHeader.biHeight = -static_cast<LONG>(output.height);
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;

  HDC screen = GetDC(nullptr);
  if (screen == nullptr) {
    return failure<core::ImageFrame>(
        core::ErrorCode::platform_error,
        L"Could not read the shell thumbnail bitmap.");
  }

  const int copied = GetDIBits(
      screen, bitmap, 0, output.height, output.pixels.data(), &info,
      DIB_RGB_COLORS);
  ReleaseDC(nullptr, screen);
  if (copied == 0) {
    return failure<core::ImageFrame>(
        core::ErrorCode::decode_error,
        L"Could not copy the shell thumbnail bitmap.");
  }

  return core::Result<core::ImageFrame>::success(std::move(output));
}

core::Result<core::ImageFrame> decode_shell_thumbnail(
    const std::filesystem::path& path,
    std::uint32_t max_edge,
    std::size_t byte_budget) {
  if (max_edge == 0) {
    return failure<core::ImageFrame>(
        core::ErrorCode::invalid_format,
        L"Thumbnail dimensions must be non-zero.");
  }

  ComPtr<IShellItemImageFactory> image_factory;
  HRESULT result = SHCreateItemFromParsingName(
      path.c_str(), nullptr, IID_PPV_ARGS(image_factory.GetAddressOf()));
  if (FAILED(result)) {
    return failure<core::ImageFrame>(
        detail::is_io_error(result) ? core::ErrorCode::io_error
                                    : core::ErrorCode::decode_error,
        L"Shell could not open the image for thumbnail extraction.");
  }

  HBITMAP bitmap = nullptr;
  SIZE size{};
  size.cx = static_cast<LONG>(max_edge);
  size.cy = static_cast<LONG>(max_edge);
  result = image_factory->GetImage(
      size, static_cast<SIIGBF>(SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK),
      &bitmap);
  if (FAILED(result) || bitmap == nullptr) {
    return failure<core::ImageFrame>(
        core::ErrorCode::decode_error,
        L"Shell could not provide a thumbnail.");
  }

  auto frame = hbitmap_to_frame(bitmap, byte_budget);
  DeleteObject(bitmap);
  return frame;
}

std::uint32_t frame_delay_ms(IWICBitmapFrameDecode& frame) {
  ComPtr<IWICMetadataQueryReader> reader;
  if (FAILED(frame.GetMetadataQueryReader(reader.GetAddressOf()))) {
    return 100;
  }

  PROPVARIANT value;
  PropVariantInit(&value);
  std::uint32_t delay = 100;
  if (SUCCEEDED(reader->GetMetadataByName(L"/grctlext/Delay", &value))) {
    if (value.vt == VT_UI2) {
      delay = static_cast<std::uint32_t>(value.uiVal) * 10U;
    } else if (value.vt == VT_UI4) {
      delay = value.ulVal * 10U;
    }
  }
  PropVariantClear(&value);
  return delay < 20U ? 100U : delay;
}

}  // namespace

namespace detail {

bool should_fallback_to_wic1(long result) noexcept {
  return result == REGDB_E_CLASSNOTREG || result == E_NOINTERFACE ||
         result == CLASS_E_CLASSNOTAVAILABLE;
}

bool is_io_error(long result) noexcept {
  return result == E_ACCESSDENIED || result == STG_E_ACCESSDENIED ||
         result == STG_E_FILENOTFOUND || result == STG_E_PATHNOTFOUND ||
         result == STG_E_SHAREVIOLATION || result == STG_E_LOCKVIOLATION ||
         result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
         result == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) ||
         result == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) ||
         result == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) ||
         result == HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION) ||
         result == HRESULT_FROM_WIN32(ERROR_INVALID_DRIVE) ||
         result == HRESULT_FROM_WIN32(ERROR_BAD_NETPATH) ||
         result == HRESULT_FROM_WIN32(ERROR_NETWORK_UNREACHABLE);
}

}  // namespace detail

core::Result<core::ImageFrame> WicDecoder::decode(
    const std::filesystem::path& path,
    std::size_t byte_budget) const {
  ComApartment apartment;
  if (!apartment.usable()) {
    return failure<core::ImageFrame>(
        core::ErrorCode::platform_error, L"COM initialization failed.");
  }

  ComPtr<IWICImagingFactory> factory;
  HRESULT result = create_factory(factory);
  if (FAILED(result)) {
    return failure<core::ImageFrame>(
        core::ErrorCode::platform_error,
        L"WIC factory creation failed.");
  }

  ComPtr<IWICBitmapDecoder> decoder;
  result = factory->CreateDecoderFromFilename(
      path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
      decoder.GetAddressOf());
  if (FAILED(result)) {
    return failure<core::ImageFrame>(
        detail::is_io_error(result) ? core::ErrorCode::io_error
                                    : core::ErrorCode::decode_error,
        L"WIC could not open the image.");
  }

  ComPtr<IWICBitmapFrameDecode> source;
  result = decoder->GetFrame(0, source.GetAddressOf());
  if (FAILED(result)) {
    return failure<core::ImageFrame>(
        core::ErrorCode::decode_error,
        L"WIC could not read frame zero.");
  }

  return decode_source_frame(*factory.Get(), *source.Get(), byte_budget);
}

core::Result<core::ImageFrame> WicDecoder::decode_thumbnail(
    const std::filesystem::path& path,
    std::uint32_t max_edge,
    std::size_t byte_budget) const {
  ComApartment apartment;
  if (!apartment.usable()) {
    return failure<core::ImageFrame>(
        core::ErrorCode::platform_error, L"COM initialization failed.");
  }

  auto shell_thumbnail = decode_shell_thumbnail(path, max_edge, byte_budget);
  if (shell_thumbnail.has_value()) {
    return shell_thumbnail;
  }

  ComPtr<IWICImagingFactory> factory;
  HRESULT result = create_factory(factory);
  if (FAILED(result)) {
    return failure<core::ImageFrame>(
        core::ErrorCode::platform_error,
        L"WIC factory creation failed.");
  }

  ComPtr<IWICBitmapDecoder> decoder;
  result = factory->CreateDecoderFromFilename(
      path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
      decoder.GetAddressOf());
  if (FAILED(result)) {
    return failure<core::ImageFrame>(
        detail::is_io_error(result) ? core::ErrorCode::io_error
                                    : core::ErrorCode::decode_error,
        L"WIC could not open the image.");
  }

  ComPtr<IWICBitmapSource> thumbnail;
  result = decoder->GetThumbnail(thumbnail.GetAddressOf());
  if (SUCCEEDED(result) && thumbnail) {
    auto decoded = decode_scaled_source_frame(*factory.Get(), *thumbnail.Get(),
                                              max_edge, byte_budget);
    if (decoded.has_value()) {
      return decoded;
    }
  }

  ComPtr<IWICBitmapFrameDecode> source;
  result = decoder->GetFrame(0, source.GetAddressOf());
  if (FAILED(result)) {
    return failure<core::ImageFrame>(
        core::ErrorCode::decode_error,
        L"WIC could not read frame zero.");
  }

  thumbnail.Reset();
  result = source->GetThumbnail(thumbnail.GetAddressOf());
  if (SUCCEEDED(result) && thumbnail) {
    auto decoded = decode_scaled_source_frame(*factory.Get(), *thumbnail.Get(),
                                              max_edge, byte_budget);
    if (decoded.has_value()) {
      return decoded;
    }
  }

  return decode_scaled_source_frame(*factory.Get(), *source.Get(), max_edge,
                                    byte_budget);
}

core::Result<core::AnimatedImage> WicDecoder::decode_animation(
    const std::filesystem::path& path,
    std::size_t byte_budget) const {
  ComApartment apartment;
  if (!apartment.usable()) {
    return failure<core::AnimatedImage>(
        core::ErrorCode::platform_error, L"COM initialization failed.");
  }

  ComPtr<IWICImagingFactory> factory;
  HRESULT result = create_factory(factory);
  if (FAILED(result)) {
    return failure<core::AnimatedImage>(
        core::ErrorCode::platform_error,
        L"WIC factory creation failed.");
  }

  ComPtr<IWICBitmapDecoder> decoder;
  result = factory->CreateDecoderFromFilename(
      path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
      decoder.GetAddressOf());
  if (FAILED(result)) {
    return failure<core::AnimatedImage>(
        detail::is_io_error(result) ? core::ErrorCode::io_error
                                    : core::ErrorCode::decode_error,
        L"WIC could not open the image.");
  }

  UINT frame_count = 0;
  result = decoder->GetFrameCount(&frame_count);
  if (FAILED(result) || frame_count == 0) {
    return failure<core::AnimatedImage>(
        core::ErrorCode::decode_error,
        L"WIC could not read animation frames.");
  }

  core::AnimatedImage animation;
  animation.frames.reserve(frame_count);
  animation.delays_ms.reserve(frame_count);
  std::size_t remaining_budget = byte_budget;
  for (UINT index = 0; index < frame_count; ++index) {
    ComPtr<IWICBitmapFrameDecode> source;
    result = decoder->GetFrame(index, source.GetAddressOf());
    if (FAILED(result)) {
      return failure<core::AnimatedImage>(
          core::ErrorCode::decode_error,
          L"WIC could not read an animation frame.");
    }

    auto frame =
        decode_source_frame(*factory.Get(), *source.Get(), remaining_budget);
    if (!frame.has_value()) {
      return core::Result<core::AnimatedImage>::failure(
          std::move(frame).error());
    }

    const std::uint64_t byte_count =
        static_cast<std::uint64_t>(frame.value().stride) *
        frame.value().height;
    if (byte_count > remaining_budget) {
      return failure<core::AnimatedImage>(
          core::ErrorCode::resource_limit,
          L"Animation pixel storage exceeds the memory budget.");
    }
    remaining_budget -= static_cast<std::size_t>(byte_count);
    animation.delays_ms.push_back(frame_delay_ms(*source.Get()));
    animation.frames.push_back(std::move(frame).value());
  }

  return core::Result<core::AnimatedImage>::success(std::move(animation));
}

}  // namespace viewer::platform
