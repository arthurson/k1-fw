#include "storage.h"
#include "../driver/lfs.h"
#include "../external/printf/printf.h"
#include "../ui/graphics.h"
#include <string.h>

// Статические буферы для кеша файлов (не используем malloc)
static uint8_t file_buffer[256]; // Размер должен быть >= lfs->cfg->cache_size

// Вспомогательный буфер
static uint8_t temp_buf[32];

bool Storage_Init(const char *name, size_t item_size, uint16_t max_items) {
  lfs_file_t file;
  struct lfs_file_config config = {.buffer = file_buffer, .attr_count = 0};

  if (lfs_file_exists(name)) {
    return false;
  }
  UI_ClearScreen();
  PrintMediumEx(LCD_XCENTER, LCD_YCENTER - 4, POS_C, C_FILL, "Creating");
  PrintMediumEx(LCD_XCENTER, LCD_YCENTER + 4, POS_C, C_FILL, "%s", name);
  ST7565_Blit();

  // Используем lfs_file_opencfg с нашим буфером
  int err = lfs_file_opencfg(&gLfs, &file, name,
                             LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC, &config);
  if (err < 0) {
    printf("[Storage_Init] Cannot create file '%s': %d\n", name, err);
    return false;
  }

  // Создаем файл нужного размера
  uint32_t total_size = max_items * item_size;
  uint32_t written = 0;

  memset(temp_buf, 0, sizeof(temp_buf));

  while (written < total_size) {
    size_t to_write = total_size - written;
    if (to_write > sizeof(temp_buf)) {
      to_write = sizeof(temp_buf);
    }

    lfs_ssize_t result = lfs_file_write(&gLfs, &file, temp_buf, to_write);
    if (result != (lfs_ssize_t)to_write) {
      printf("[Storage_Init] Write failed: %ld\n", result);
      lfs_file_close(&gLfs, &file);
      return false;
    }
    written += to_write;
  }

  lfs_file_close(&gLfs, &file);
  printf("[Storage_Init] File '%s' created, size: %lu\n", name, total_size);

  gRedrawScreen = true;

  return true;
}

bool Storage_Save(const char *name, uint16_t num, const void *item,
                  size_t item_size) {
  lfs_file_t file;
  struct lfs_file_config config = {.buffer = file_buffer, .attr_count = 0};

  // Открываем с нашим буфером
  int err =
      lfs_file_opencfg(&gLfs, &file, name, LFS_O_RDWR | LFS_O_CREAT, &config);
  if (err < 0) {
    printf("[Storage_Save] Cannot open file '%s': %d\n", name, err);
    return false;
  }

  // Узнаем текущий размер
  lfs_soff_t file_size = lfs_file_size(&gLfs, &file);
  if (file_size < 0) {
    printf("[Storage_Save] Cannot get file size\n");
    lfs_file_close(&gLfs, &file);
    return false;
  }

  uint32_t offset = num * item_size;
  uint32_t required_size = offset + item_size;

  printf("[Storage_Save] file=%s num=%u offset=%lu file_size=%ld required=%lu\n",
         name, num, offset, file_size, required_size);

  // Если нужно расширить файл
  if (required_size > (uint32_t)file_size) {
    printf("[Storage_Save] Extending file by %lu bytes\n", required_size - file_size);
    // Переходим в конец
    if (lfs_file_seek(&gLfs, &file, 0, LFS_SEEK_END) < 0) {
      printf("[Storage_Save] Seek to end failed\n");
      lfs_file_close(&gLfs, &file);
      return false;
    }

    // Расширяем файл нулями
    uint32_t to_extend = required_size - file_size;
    memset(temp_buf, 0, sizeof(temp_buf));

    while (to_extend > 0) {
      size_t chunk = to_extend;
      if (chunk > sizeof(temp_buf))
        chunk = sizeof(temp_buf);

      lfs_ssize_t written = lfs_file_write(&gLfs, &file, temp_buf, chunk);
      if (written != (lfs_ssize_t)chunk) {
        printf("[Storage_Save] Extend failed: %ld\n", written);
        lfs_file_close(&gLfs, &file);
        return false;
      }
      to_extend -= chunk;
    }

    // Возвращаемся к началу для записи данных
    if (lfs_file_seek(&gLfs, &file, offset, LFS_SEEK_SET) < 0) {
      printf("[Storage_Save] Seek failed after extend\n");
      lfs_file_close(&gLfs, &file);
      return false;
    }
  } else {
    // Просто переходим к позиции
    if (lfs_file_seek(&gLfs, &file, offset, LFS_SEEK_SET) < 0) {
      printf("[Storage_Save] Seek failed\n");
      lfs_file_close(&gLfs, &file);
      return false;
    }
  }

  // Записываем данные
  lfs_ssize_t written = lfs_file_write(&gLfs, &file, item, item_size);
  lfs_file_close(&gLfs, &file);

  if (written != (lfs_ssize_t)item_size) {
    printf("[Storage_Save] Write failed: %ld/%zu\n", written, item_size);
    return false;
  }

  printf("[Storage_Save] OK: wrote %ld bytes at offset %lu\n", written, offset);
  return true;
}

bool Storage_Load(const char *name, uint16_t num, void *item,
                  size_t item_size) {
  lfs_file_t file;
  struct lfs_file_config config = {.buffer = file_buffer, .attr_count = 0};

  int err = lfs_file_opencfg(&gLfs, &file, name, LFS_O_RDONLY, &config);
  if (err < 0) {
    printf("[Storage_Load] Cannot open file '%s': %d\n", name, err);
    return false;
  }

  // Проверяем размер
  lfs_soff_t file_size = lfs_file_size(&gLfs, &file);
  if (file_size < 0) {
    printf("[Storage_Load] Cannot get file size\n");
    lfs_file_close(&gLfs, &file);
    return false;
  }

  uint32_t offset = num * item_size;
  uint32_t required_size = offset + item_size;

  if (required_size > (uint32_t)file_size) {
    printf("[Storage_Load] Offset %lu > file size %ld\n", required_size,
           file_size);
    lfs_file_close(&gLfs, &file);
    return false;
  }

  // Переходим к позиции
  if (lfs_file_seek(&gLfs, &file, offset, LFS_SEEK_SET) < 0) {
    printf("[Storage_Load] Seek failed\n");
    lfs_file_close(&gLfs, &file);
    return false;
  }

  // Читаем данные
  lfs_ssize_t read = lfs_file_read(&gLfs, &file, item, item_size);
  lfs_file_close(&gLfs, &file);

  if (read != (lfs_ssize_t)item_size) {
    printf("[Storage_Load] Read failed: %ld/%zu\n", read, item_size);
    return false;
  }

  return true;
}

// Дополнительные функции
bool Storage_Exists(const char *name) {
  struct lfs_info info;
  return lfs_stat(&gLfs, name, &info) == 0;
}

bool Storage_LoadMultiple(const char *name, uint16_t start_num, void *items,
                          size_t item_size, uint16_t count) {
  if (count == 0) {
    return true;
  }

  lfs_file_t file;
  struct lfs_file_config config = {.buffer = file_buffer, .attr_count = 0};

  int err = lfs_file_opencfg(&gLfs, &file, name, LFS_O_RDONLY, &config);
  if (err < 0) {
    printf("[Storage_LoadMultiple] Cannot open file '%s': %d\n", name, err);
    return false;
  }

  // Проверяем размер файла
  lfs_soff_t file_size = lfs_file_size(&gLfs, &file);
  if (file_size < 0) {
    printf("[Storage_LoadMultiple] Cannot get file size\n");
    lfs_file_close(&gLfs, &file);
    return false;
  }

  uint32_t offset = start_num * item_size;
  uint32_t total_size = count * item_size;
  uint32_t required_size = offset + total_size;

  if (required_size > (uint32_t)file_size) {
    printf("[Storage_LoadMultiple] Offset %lu > file size %ld\n", required_size,
           file_size);
    lfs_file_close(&gLfs, &file);
    return false;
  }

  // Seek к начальной позиции ОДИН раз
  if (lfs_file_seek(&gLfs, &file, offset, LFS_SEEK_SET) < 0) {
    printf("[Storage_LoadMultiple] Seek failed\n");
    lfs_file_close(&gLfs, &file);
    return false;
  }

  // Читаем ВСЕ элементы ОДНИМ вызовом
  lfs_ssize_t read = lfs_file_read(&gLfs, &file, items, total_size);
  lfs_file_close(&gLfs, &file);

  if (read != (lfs_ssize_t)total_size) {
    printf("[Storage_LoadMultiple] Read failed: %ld/%lu\n", read, total_size);
    return false;
  }

  printf("[Storage_LoadMultiple] Loaded %u items in one read\n", count);
  return true;
}

// ============================================================================
// НОВАЯ ФУНКЦИЯ: Пакетное сохранение нескольких элементов
// ============================================================================
bool Storage_SaveMultiple(const char *name, uint16_t start_num,
                          const void *items, size_t item_size, uint16_t count) {
  if (count == 0) {
    return true;
  }

  lfs_file_t file;
  struct lfs_file_config config = {.buffer = file_buffer, .attr_count = 0};

  int err =
      lfs_file_opencfg(&gLfs, &file, name, LFS_O_RDWR | LFS_O_CREAT, &config);
  if (err < 0) {
    printf("[Storage_SaveMultiple] Cannot open file '%s': %d\n", name, err);
    return false;
  }

  lfs_soff_t file_size = lfs_file_size(&gLfs, &file);
  if (file_size < 0) {
    printf("[Storage_SaveMultiple] Cannot get file size\n");
    lfs_file_close(&gLfs, &file);
    return false;
  }

  uint32_t offset = start_num * item_size;
  uint32_t total_size = count * item_size;
  uint32_t required_size = offset + total_size;

  // Расширяем файл если нужно
  if (required_size > (uint32_t)file_size) {
    if (lfs_file_seek(&gLfs, &file, 0, LFS_SEEK_END) < 0) {
      lfs_file_close(&gLfs, &file);
      return false;
    }

    uint32_t to_extend = required_size - file_size;
    memset(temp_buf, 0, sizeof(temp_buf));

    while (to_extend > 0) {
      size_t chunk = to_extend;
      if (chunk > sizeof(temp_buf))
        chunk = sizeof(temp_buf);

      lfs_ssize_t written = lfs_file_write(&gLfs, &file, temp_buf, chunk);
      if (written != (lfs_ssize_t)chunk) {
        lfs_file_close(&gLfs, &file);
        return false;
      }
      to_extend -= chunk;
    }
  }

  // Seek к начальной позиции
  if (lfs_file_seek(&gLfs, &file, offset, LFS_SEEK_SET) < 0) {
    lfs_file_close(&gLfs, &file);
    return false;
  }

  // Пишем ВСЕ элементы ОДНИМ вызовом
  lfs_ssize_t written = lfs_file_write(&gLfs, &file, items, total_size);
  lfs_file_close(&gLfs, &file);

  if (written != (lfs_ssize_t)total_size) {
    printf("[Storage_SaveMultiple] Write failed: %ld/%lu\n", written,
           total_size);
    return false;
  }

  printf("[Storage_SaveMultiple] Saved %u items in one write\n", count);
  return true;
}
