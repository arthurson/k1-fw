#include "messenger.h"
#include "../driver/bk4829.h"
#include "../driver/systick.h"
#include "../external/printf/printf.h"
#include "../helper/bands.h"
#include "../helper/fsk2.h"
#include "../helper/measurements.h"
#include "../radio.h"
#include "../settings.h"
#include "../ui/finput.h"
#include "../ui/graphics.h"
#include "../ui/statusline.h"
#include "../ui/textinput.h"
#include "../ui/toast.h"
#include "apps.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// Максимальное количество сообщений в истории
#define MAX_MESSAGES 4

bool gHasUnreadMessages;

// Структура для сообщений
typedef struct {
  bool incoming;
  char text[FSK_LEN * 2 + 1]; // Два байта на uint16_t + нуль-терминатор
} Message_t;

static Message_t messages[MAX_MESSAGES] = {0};
static uint8_t msgCount = 0;
static uint8_t scrollOffset = 0; // Для скролла по истории
//
static void tuneTo(uint32_t f, uint32_t _) {
  (void)_;
  RADIO_SetParam(ctx, PARAM_FREQUENCY, f, true);
  RADIO_ApplySettings(ctx);
}

// Функция для упаковки текста в FSK_TXDATA
static void packTextToFsk(const char *text) {
  memset(FSK_TXDATA, 0, sizeof(FSK_TXDATA));
  size_t len = strlen(text);
  for (size_t i = 0; i < FSK_LEN && i * 2 < len; i++) {
    FSK_TXDATA[i] = (uint16_t)text[i * 2] | ((uint16_t)text[i * 2 + 1] << 8);
  }
}

// Функция для распаковки FSK_RXDATA в текст
static void unpackFskToText(char *text) {
  memset(text, 0, FSK_LEN * 2 + 1);
  for (size_t i = 0; i < FSK_LEN; i++) {
    text[i * 2] = (char)(FSK_RXDATA[i] & 0xFF);
    text[i * 2 + 1] = (char)((FSK_RXDATA[i] >> 8) & 0xFF);
  }
}

// Callback после ввода текста: отправка сообщения
static void sendMessageCallback(void) {
  TOAST_Push("GOT: %s", gTextinputText);
  // Добавить в историю как исходящее
  if (msgCount < MAX_MESSAGES) {
    messages[msgCount].incoming = false;
    strncpy(messages[msgCount].text, gTextinputText, FSK_LEN * 2);
    msgCount++;
  } else {
    // Сдвинуть историю
    memmove(messages, messages + 1, sizeof(Message_t) * (MAX_MESSAGES - 1));
    messages[MAX_MESSAGES - 1].incoming = false;
    strncpy(messages[MAX_MESSAGES - 1].text, gTextinputText, FSK_LEN * 2);
  }

  // Упаковать и отправить
  packTextToFsk(gTextinputText);
  RADIO_ToggleTX(ctx, true);
  RF_EnterFsk();
  RF_FskTransmit();
  RADIO_ToggleTX(ctx, false);

  // Обновить экран
  gRedrawScreen = true;
}

void MESSENGER_init(void) {
  // Инициализация FSK режима для приёма/передачи
  RF_EnterFsk();

  // Настроить радио на частоту для мессенджера (пример: взять из текущего VFO)
  RADIO_SetParam(ctx, PARAM_FREQUENCY, 433000000,
                 true); // Пример 433 MHz, изменить по нужде
  RADIO_ApplySettings(ctx);

  // Инициализация истории
  /* memset(messages, 0, sizeof(messages));
  msgCount = 0;
  scrollOffset = 0; */

  // Подготовка для ввода текста
  gTextInputSize = FSK_LEN * 2; // Максимальная длина сообщения
}

void MESSENGER_deinit(void) {
  // TODO: exit fsk?
}

void MESSENGER_update(void) {
  // Проверяем, получено ли новое сообщение
  if (gNewFskMessage) {
    gNewFskMessage = false; // Сброс флага

    char receivedText[FSK_LEN * 2 + 1];
    unpackFskToText(receivedText);

    // Добавить в историю как входящее
    if (msgCount < MAX_MESSAGES) {
      messages[msgCount].incoming = true;
      strncpy(messages[msgCount].text, receivedText, FSK_LEN * 2);
      msgCount++;
    } else {
      // Сдвинуть историю
      memmove(messages, messages + 1, sizeof(Message_t) * (MAX_MESSAGES - 1));
      messages[MAX_MESSAGES - 1].incoming = true;
      strncpy(messages[MAX_MESSAGES - 1].text, receivedText, FSK_LEN * 2);
    }

    // Обновить экран
    gRedrawScreen = true;
  }
}

bool MESSENGER_key(KEY_Code_t key, Key_State_t state) {
  if (state == KEY_RELEASED) {
    switch (key) {
    case KEY_STAR:
      // Ввод нового сообщения
      TEXTINPUT_Show(sendMessageCallback, 15);
      return true;

    case KEY_EXIT:
      // Выход из приложения
      RF_ExitFsk(); // Выход из FSK режима
      APPS_exit();
      return true;

    case KEY_UP:
      // Скролл вверх
      if (scrollOffset > 0) {
        scrollOffset--;
        gRedrawScreen = true;
      }
      return true;

    case KEY_DOWN:
      // Скролл вниз
      if (scrollOffset + 5 < msgCount) { // Предполагаем 5 строк на экране
        scrollOffset++;
        gRedrawScreen = true;
      }
      return true;
    case KEY_5:
      FINPUT_setup(BK4819_F_MIN, BK4819_F_MAX, UNIT_MHZ, false);
      FINPUT_Show(tuneTo);
      return true;

    default:
      break;
    }
  }
  return false;
}

void MESSENGER_render(void) {
  gHasUnreadMessages = false;
  // Заголовок
  PrintMediumEx(LCD_XCENTER, 14, POS_C, C_FILL, "%s",
                RADIO_GetParamValueString(ctx, PARAM_FREQUENCY));

  // Отображение истории сообщений (последние 5, с учетом скролла)
  uint8_t visibleCount = (msgCount > 5) ? 5 : msgCount;
  uint8_t startIdx =
      (msgCount > visibleCount) ? msgCount - visibleCount - scrollOffset : 0;

  for (uint8_t i = 0; i < visibleCount; i++) {
    uint8_t idx = startIdx + i;
    if (idx >= msgCount)
      break;

    const char *prefix = messages[idx].incoming ? "RX: " : "TX: ";
    PrintSmallEx(0, 24 + i * 8, POS_L, C_FILL, "%s%s", prefix,
                 messages[idx].text);
  }

  // Подсказки
  PrintSmallEx(0, LCD_HEIGHT - 2, POS_L, C_FILL, "*: New msg | EXIT: Quit");
}
