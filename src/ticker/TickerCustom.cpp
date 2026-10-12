#include <Arduino.h>
#include <Bitmap.h>
#include <BoardConfig.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PngToBmpConverter.h>

#include <cstring>

#include "Ticker.h"
#include "network/HttpDownloader.h"

namespace ticker {
namespace {
constexpr char DOWNLOAD_PATH[] = "/.crosspoint/ticker_custom.dl";
constexpr char CONVERTED_PATH[] = "/.crosspoint/ticker_custom.tmp";

enum class ImageKind { Unknown, Png, Bmp };

ImageKind sniff(const char* path) {
  HalFile f;
  if (!Storage.openFileForRead("TICK", path, f)) return ImageKind::Unknown;
  uint8_t head[8] = {};
  if (f.read(head, sizeof(head)) != sizeof(head)) return ImageKind::Unknown;
  static constexpr uint8_t PNG_SIG[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (memcmp(head, PNG_SIG, sizeof(PNG_SIG)) == 0) return ImageKind::Png;
  if (head[0] == 'B' && head[1] == 'M') return ImageKind::Bmp;
  return ImageKind::Unknown;
}

bool validBmp(const char* path) {
  HalFile f;
  if (!Storage.openFileForRead("TICK", path, f)) return false;
  Bitmap bmp(f);
  return bmp.parseHeaders() == BmpReaderError::Ok;
}

// Replaces the current image only once the new one is known good.
bool promote(const char* from) {
  if (!validBmp(from)) {
    LOG_ERR("TICK", "Custom image is not a usable BMP");
    return false;
  }
  Storage.remove(CUSTOM_IMAGE_PATH);
  return Storage.rename(from, CUSTOM_IMAGE_PATH);
}
}  // namespace

bool hasCustomImage() { return Storage.exists(CUSTOM_IMAGE_PATH); }

bool downloadCustomImage(const Config& cfg) {
  if (!customEnabled(cfg)) return false;
  const bool tls = strncmp(cfg.customUrl, "https://", 8) == 0;
  if (tls &&
      (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP || ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC)) {
    LOG_ERR("TICK", "Not enough heap for TLS: %u free, %u max", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    setLastError(FetchError::Memory);
    return false;
  }
  Storage.remove(DOWNLOAD_PATH);
  if (HttpDownloader::downloadToFile(cfg.customUrl, DOWNLOAD_PATH) != HttpDownloader::OK) {
    LOG_ERR("TICK", "Custom image download failed");
    Storage.remove(DOWNLOAD_PATH);
    setLastError(FetchError::Network);
    return false;
  }
  return true;
}

bool prepareCustomImage(const Config& cfg) {
  if (!Storage.exists(DOWNLOAD_PATH)) return false;
  const unsigned long start = millis();
  bool ok = false;
  switch (sniff(DOWNLOAD_PATH)) {
    case ImageKind::Bmp:
      ok = promote(DOWNLOAD_PATH);
      break;
    case ImageKind::Png: {
      // Convert at the logical screen size of the configured orientation.
      const int nativeW = BoardConfig::ACTIVE.displayWidth;
      const int nativeH = BoardConfig::ACTIVE.displayHeight;
      const int longSide = nativeW > nativeH ? nativeW : nativeH;
      const int shortSide = nativeW > nativeH ? nativeH : nativeW;
      const bool portrait = cfg.orientation == PORTRAIT;
      const int w = portrait ? shortSide : longSide;
      const int h = portrait ? longSide : shortSide;
      bool converted = false;
      {
        HalFile png, out;
        if (Storage.openFileForRead("TICK", DOWNLOAD_PATH, png) && Storage.openFileForWrite("TICK", CONVERTED_PATH, out)) {
          converted = PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(png, out, w, h);
        }
      }
      if (!converted) LOG_ERR("TICK", "PNG conversion failed (free heap %u, max block %u)", ESP.getFreeHeap(),
                              ESP.getMaxAllocHeap());
      ok = converted && promote(CONVERTED_PATH);
      break;
    }
    case ImageKind::Unknown:
    default:
      LOG_ERR("TICK", "Custom URL did not return a PNG or BMP");
      break;
  }
  Storage.remove(DOWNLOAD_PATH);
  Storage.remove(CONVERTED_PATH);
  setLastError(ok ? FetchError::None : FetchError::Data);
  LOG_INF("TICK", "Custom image %s in %lu ms", ok ? "updated" : "failed", millis() - start);
  return ok;
}

}  // namespace ticker
