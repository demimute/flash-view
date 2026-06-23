#include "viewer/platform/wic_decoder.h"

#include <Windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <utility>

namespace viewer::platform {
namespace {

using Microsoft::WRL::ComPtr;

core::Result<core::ImageFrame> failure(core::ErrorCode code,
                                       const wchar_t* message) {
  return core::Result<core::ImageFrame>::failure({code, message});
}

bool is_io_error(HRESULT result) {
  return result == E_ACCESSDENIED || result == STG_E_ACCESSDENIED ||
         result == STG_E_FILENOTFOUND || result == STG_E_PATHNOTFOUND ||
         result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
         result == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) ||
         result == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) ||
         result == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) ||
         result == HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION) ||
         result == HRESULT_FROM_WIN32(ERROR_INVALID_DRIVE) ||
         result == HRESULT_FROM_WIN32(ERROR_BAD_NETPATH) ||
         result == HRESULT_FROM_WIN32(ERROR_NETWORK_UNREACHABLE);
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

  factory.Reset();
  return CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                          CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(factory.GetAddressOf()));
}

}  // namespace

core::Result<core::ImageFrame> WicDecoder::decode(
    const std::filesystem::path& path,
    std::size_t byte_budget) const {
  ComApartment apartment;
  if (!apartment.usable()) {
    return failure(core::ErrorCode::platform_error,
                   L"COM initialization failed.");
  }

  ComPtr<IWICImagingFactory> factory;
  HRESULT result = create_factory(factory);
  if (FAILED(result)) {
    return failure(core::ErrorCode::platform_error,
                   L"WIC factory creation failed.");
  }

  ComPtr<IWICBitmapDecoder> decoder;
  result = factory->CreateDecoderFromFilename(
      path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
      decoder.GetAddressOf());
  if (FAILED(result)) {
    return failure(is_io_error(result) ? core::ErrorCode::io_error
                                       : core::ErrorCode::decode_error,
                   L"WIC could not open the image.");
  }

  ComPtr<IWICBitmapFrameDecode> source;
  result = decoder->GetFrame(0, source.GetAddressOf());
  if (FAILED(result)) {
    return failure(core::ErrorCode::decode_error,
                   L"WIC could not read frame zero.");
  }

  UINT width = 0;
  UINT height = 0;
  result = source->GetSize(&width, &height);
  if (FAILED(result)) {
    return failure(core::ErrorCode::decode_error,
                   L"WIC could not read image dimensions.");
  }

  auto frame = core::ImageFrame::allocate_bgra8(width, height, byte_budget);
  if (!frame.has_value()) {
    return frame;
  }

  ComPtr<IWICFormatConverter> converter;
  result = factory->CreateFormatConverter(converter.GetAddressOf());
  if (FAILED(result)) {
    return failure(core::ErrorCode::platform_error,
                   L"WIC format converter creation failed.");
  }

  result = converter->Initialize(
      source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
      nullptr, 0.0, WICBitmapPaletteTypeCustom);
  if (FAILED(result)) {
    return failure(core::ErrorCode::decode_error,
                   L"WIC pixel conversion failed.");
  }

  auto& output = frame.value();
  const UINT buffer_size = output.stride * output.height;
  result = converter->CopyPixels(
      nullptr, output.stride, buffer_size,
      reinterpret_cast<BYTE*>(output.pixels.data()));
  if (FAILED(result)) {
    return failure(core::ErrorCode::decode_error,
                   L"WIC pixel copy failed.");
  }

  return core::Result<core::ImageFrame>::success(std::move(output));
}

}  // namespace viewer::platform
