#include "screenshot.h"
#include "../driver/lfs.h"
#include "../driver/uart.h"
#include "../ui/graphics.h"
#include <stdlib.h>

// Структура BMP заголовка для 1-битного изображения
#pragma pack(push, 1)
typedef struct {
  // File header
  uint16_t bfType; // 'BM'
  uint32_t bfSize; // File size
  uint16_t bfReserved1;
  uint16_t bfReserved2;
  uint32_t bfOffBits; // Offset to pixel data

  // DIB header
  uint32_t biSize; // Header size (40)
  int32_t biWidth;
  int32_t biHeight;
  uint16_t biPlanes;      // = 1
  uint16_t biBitCount;    // = 1
  uint32_t biCompression; // = 0 (BI_RGB)
  uint32_t biSizeImage;
  int32_t biXPelsPerMeter;
  int32_t biYPelsPerMeter;
  uint32_t biClrUsed;
  uint32_t biClrImportant;

  // Color palette (for 1-bit)
  uint32_t palette[2];
} BMPFile;
#pragma pack(pop)

// Статические функции
static bool captureScreenToBMP(const char *filename);
static bool displayBMPFromLittleFS(const char *filename);

// Публичные функции
bool captureScreen(void);
bool displayScreen(const char *filename);
void listScreenshots(void);

// ============================================
// Вспомогательные функции
// ============================================

static int writeFile(lfs_t *lfs, const char *path, const void *buffer,
                     lfs_size_t size) {
  lfs_file_t file;
  uint8_t file_buffer[256];
  struct lfs_file_config config = {.buffer = file_buffer, .attr_count = 0};
  int err =
      lfs_file_opencfg(lfs, &file, path, LFS_O_WRONLY | LFS_O_CREAT, &config);
  if (err) {
    Log("Failed to open file: %s, err: %d", path, err);
    return err;
  }

  lfs_ssize_t written = lfs_file_write(lfs, &file, buffer, size);
  lfs_file_close(lfs, &file);

  if (written != (lfs_ssize_t)size) {
    Log("Failed to write file: %s", path);
    return LFS_ERR_IO;
  }

  return LFS_ERR_OK;
}

static int readFile(lfs_t *lfs, const char *path, void *buffer,
                    lfs_size_t size) {
  lfs_file_t file;
  uint8_t file_buffer[256];
  struct lfs_file_config config = {.buffer = file_buffer, .attr_count = 0};
  int err = lfs_file_opencfg(lfs, &file, path, LFS_O_RDONLY, &config);
  if (err) {
    Log("File not found: %s, err: %d", path, err);
    return err;
  }

  lfs_ssize_t read = lfs_file_read(lfs, &file, buffer, size);
  lfs_file_close(lfs, &file);

  if (read != (lfs_ssize_t)size) {
    Log("Failed to read file: %s", path);
    return LFS_ERR_IO;
  }

  return LFS_ERR_OK;
}

static bool fileExists(lfs_t *lfs, const char *path) {
  struct lfs_info info;
  return lfs_stat(lfs, path, &info) == 0;
}

// ============================================
// Захват экрана в BMP
// ============================================

static bool captureScreenToBMP(const char *filename) {
  const int width = LCD_WIDTH;
  const int height = LCD_HEIGHT;

  // Calculate row size (BMP rows are padded to 4 bytes)
  int rowSize = ((width + 31) / 32) * 4;
  int imageSize = rowSize * height;
  int fileSize = sizeof(BMPFile) + imageSize;

  // Initialize BMP header
  BMPFile bmp = {0};

  // File header
  bmp.bfType = 0x4D42; // 'BM'
  bmp.bfSize = fileSize;
  bmp.bfOffBits = sizeof(BMPFile);

  // DIB header
  bmp.biSize = 40;
  bmp.biWidth = width;
  bmp.biHeight = height; // Positive = bottom-to-top
  bmp.biPlanes = 1;
  bmp.biBitCount = 1;    // 1-bit monochrome
  bmp.biCompression = 0; // BI_RGB
  bmp.biSizeImage = imageSize;
  bmp.biXPelsPerMeter = 2835; // ~72 DPI
  bmp.biYPelsPerMeter = 2835;
  bmp.biClrUsed = 2;
  bmp.biClrImportant = 2;

  // Color palette (white, black)
  bmp.palette[0] = 0xFFFFFF; // Color 0: white
  bmp.palette[1] = 0x000000; // Color 1: black

  // Write header
  if (writeFile(&gLfs, filename, &bmp, sizeof(bmp)) != LFS_ERR_OK) {
    return false;
  }

  // Open file for appending pixel data
  lfs_file_t file;
  uint8_t file_buffer[256];
  struct lfs_file_config config = {.buffer = file_buffer, .attr_count = 0};
  int err = lfs_file_opencfg(&gLfs, &file, filename,
                             LFS_O_WRONLY | LFS_O_APPEND, &config);
  if (err) {
    Log("Failed to open for append: %s", filename);
    return false;
  }

  // Allocate buffer for one row
  uint8_t rowBuffer[rowSize];

  // Write pixel data (bottom row first - BMP format)
  for (int32_t y = height - 1; y >= 0; y--) {
    // Clear row buffer
    for (int i = 0; i < rowSize; i++) {
      rowBuffer[i] = 0;
    }

    // Pack pixels into bytes (8 pixels per byte)
    for (int x = 0; x < width; x++) {
      // Get pixel (1 = black, 0 = white)
      uint8_t pixel = GetPixel(x, y) ? 1 : 0;

      // Calculate position in buffer
      int bytePos = x / 8;
      int bitPos = 7 - (x % 8); // MSB first

      // Set the bit
      if (pixel) {
        rowBuffer[bytePos] |= (1 << bitPos);
      }
    }

    // Write row
    lfs_ssize_t written = lfs_file_write(&gLfs, &file, rowBuffer, rowSize);
    if (written != rowSize) {
      Log("Failed to write row data");
      lfs_file_close(&gLfs, &file);
      return false;
    }
  }

  lfs_file_close(&gLfs, &file);
  Log("Screenshot saved: %s (%d bytes)", filename, fileSize);
  return true;
}

bool captureScreen(void) {
  // Создаем директорию если нужно
  struct lfs_info info;
  if (lfs_stat(&gLfs, "/Screenshots", &info) < 0) {
    int err = lfs_mkdir(&gLfs, "/Screenshots");
    if (err) {
      Log("Failed to create Screenshots dir: %d", err);
      return false;
    }
  }

  // Ищем свободный номер файла
  for (int i = 1; i < 1000; i++) {
    char filename[32];
    snprintf(filename, sizeof(filename), "/Screenshots/screen_%03d.bmp", i);

    if (!fileExists(&gLfs, filename)) {
      return captureScreenToBMP(filename);
    }
  }

  Log("Too many screenshots!");
  return false;
}

// ============================================
// Отображение BMP из LittleFS
// ============================================

static bool displayBMPFromLittleFS(const char *filename) {
  // Читаем заголовок
  BMPFile bmp;
  if (readFile(&gLfs, filename, &bmp, sizeof(bmp)) != LFS_ERR_OK) {
    return false;
  }

  // Проверяем что это валидный 1-битный BMP
  if (bmp.bfType != 0x4D42 || bmp.biBitCount != 1) {
    Log("Not a 1-bit BMP: %s", filename);
    return false;
  }

  // Рассчитываем размер строки
  int width = bmp.biWidth;
  int height = abs(bmp.biHeight); // Обрабатываем отрицательную высоту
  int rowSize = ((width + 31) / 32) * 4;

  // Определяем направление (высота может быть отрицательной)
  bool bottomToTop = bmp.biHeight > 0;

  // Открываем файл для чтения данных пикселей
  lfs_file_t file;
  uint8_t file_buffer[256];
  struct lfs_file_config config = {.buffer = file_buffer, .attr_count = 0};
  int err = lfs_file_opencfg(&gLfs, &file, filename, LFS_O_RDONLY, &config);
  if (err) {
    Log("Failed to open file: %s", filename);
    return false;
  }

  // Переходим к данным пикселей
  lfs_file_seek(&gLfs, &file, bmp.bfOffBits, LFS_SEEK_SET);

  // Буфер для одной строки
  uint8_t rowBuffer[rowSize];

  // Читаем и отображаем изображение
  for (int y = 0; y < height; y++) {
    // Читаем строку
    lfs_ssize_t read = lfs_file_read(&gLfs, &file, rowBuffer, rowSize);
    if (read != rowSize) {
      Log("Error reading pixel data at row %d", y);
      lfs_file_close(&gLfs, &file);
      return false;
    }

    // Вычисляем целевую Y позицию
    int targetY = bottomToTop ? (height - 1 - y) : y;

    // Распаковываем пиксели
    for (int x = 0; x < width; x++) {
      int bytePos = x / 8;
      int bitPos = 7 - (x % 8);

      // Получаем значение пикселя из BMP (1 = черный, 0 = белый)
      uint8_t pixel = (rowBuffer[bytePos] >> bitPos) & 1;

      PutPixel(x, targetY, pixel ? C_FILL : C_CLEAR);
    }
  }

  lfs_file_close(&gLfs, &file);
  Log("Displayed: %s", filename);
  return true;
}

bool displayScreen(const char *filename) {
  return displayBMPFromLittleFS(filename);
}
