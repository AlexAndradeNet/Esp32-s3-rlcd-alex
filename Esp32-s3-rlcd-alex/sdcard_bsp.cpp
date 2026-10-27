#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include "sdcard_bsp.h"

CustomSDPort::CustomSDPort(const char *SdName, int clk, int cmd, int d0, int width)
  : SdName_(SdName) {
  mount_config_ = {};
  mount_config_.format_if_mount_failed = false;
  mount_config_.max_files = 5;
  mount_config_.allocation_unit_size = 16 * 1024 * 3;

  host_ = SDMMC_HOST_DEFAULT();
  // 10 MHz instead of the 20 MHz default: more timing margin against rail noise from Wi-Fi bursts
  // (0x107 timeouts). 1-bit at 10 MHz is ~1.2 MB/s, far above the 32 KB/s a recording needs.
  host_.max_freq_khz = 10000;

  slot_config_ = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config_.width = width;
  slot_config_.clk = (gpio_num_t)clk;
  slot_config_.cmd = (gpio_num_t)cmd;
  slot_config_.d0 = (gpio_num_t)d0;

  Mount();
}

bool CustomSDPort::Mount() {
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_vfs_fat_sdmmc_mount(SdName_, &host_, &slot_config_, &mount_config_, &sdcard_host));

  if (sdcard_host != NULL) {
    sdmmc_card_print_info(stdout, sdcard_host);
    is_SdcardInitOK = 1;
  } else {
    is_SdcardInitOK = 0;
  }
  return is_SdcardInitOK;
}

// FATFS keeps failing once the card has stopped answering (0x107 timeouts), until the card is
// initialized again. Unmounting also de-initializes the SDMMC host, so Mount() starts from scratch.
bool CustomSDPort::SDPort_EnsureReady(bool forceRemount) {
  if (!forceRemount && sdcard_host != NULL && sdmmc_get_status(sdcard_host) == ESP_OK) return true;

  ESP_LOGW(TAG, "%s", forceRemount ? "Remounting SD card after an error" : "SD card not responding, remounting");
  if (sdcard_host != NULL) {
    esp_vfs_fat_sdcard_unmount(SdName_, sdcard_host);
    sdcard_host = NULL;
  }
  return Mount();
}

CustomSDPort::~CustomSDPort() {
}

int CustomSDPort::SDPort_WriteFile(const char *path, const void *data, size_t data_len) {
  if (sdcard_host == NULL) {
    ESP_LOGE(TAG, "SD card not initialized");
    return ESP_ERR_INVALID_STATE;
  }

  if (sdmmc_get_status(sdcard_host) != ESP_OK) {
    ESP_LOGE(TAG, "SD card not ready");
    return ESP_FAIL;
  }

  FILE *f = fopen(path, "wb");
  if (f == NULL) {
    ESP_LOGE(TAG, "Failed to open file for writing: %s", path);
    return ESP_ERR_NOT_FOUND;
  }

  size_t written = fwrite(data, 1, data_len, f);
  fclose(f);

  if (written != data_len) {
    ESP_LOGE(TAG, "Write failed (%zu/%zu bytes)", written, data_len);
    return ESP_FAIL;
  }
  return ESP_OK;
}

int CustomSDPort::SDPort_ReadFile(const char *path, uint8_t *buffer, size_t *outLen) {
  if (sdcard_host == NULL) {
    ESP_LOGE(TAG, "SD card not initialized");
    return ESP_ERR_INVALID_STATE;
  }

  if (sdmmc_get_status(sdcard_host) != ESP_OK) {
    ESP_LOGE(TAG, "SD card not ready");
    return ESP_FAIL;
  }

  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    ESP_LOGE(TAG, "Failed to open file: %s", path);
    return ESP_ERR_NOT_FOUND;
  }

  fseek(f, 0, SEEK_END);
  long file_size = ftell(f);
  if (file_size <= 0) {
    ESP_LOGE(TAG, "Invalid file size");
    fclose(f);
    return ESP_FAIL;
  }
  fseek(f, 0, SEEK_SET);

  size_t bytes_read = fread(buffer, 1, file_size, f);
  fclose(f);

  if (outLen) *outLen = bytes_read;
  return (bytes_read > 0) ? ESP_OK : ESP_FAIL;
}

int CustomSDPort::SDPort_ReadOffset(const char *path, void *buffer, size_t len, size_t offset) {
  if (sdcard_host == NULL) {
    ESP_LOGE(TAG, "SD card not initialized");
    return ESP_ERR_INVALID_STATE;
  }

  if (sdmmc_get_status(sdcard_host) != ESP_OK) {
    ESP_LOGE(TAG, "SD card not ready");
    return ESP_FAIL;
  }

  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    ESP_LOGE(TAG, "Failed to open file: %s", path);
    return ESP_ERR_NOT_FOUND;
  }

  fseek(f, offset, SEEK_SET);
  size_t bytes_read = fread(buffer, 1, len, f);
  fclose(f);
  return bytes_read;
}

int CustomSDPort::SDPort_WriteOffset(const char *path, const void *data, size_t len, bool append) {
  if (sdcard_host == NULL) {
    ESP_LOGE(TAG, "SD card not initialized");
    return ESP_ERR_INVALID_STATE;
  }

  if (sdmmc_get_status(sdcard_host) != ESP_OK) {
    ESP_LOGE(TAG, "SD card not ready");
    return ESP_FAIL;
  }

  const char *mode = append ? "ab" : "wb";
  FILE *f = fopen(path, mode);
  if (f == NULL) {
    ESP_LOGE(TAG, "Failed to open file: %s", path);
    return ESP_ERR_NOT_FOUND;
  }

  size_t bytes_written = fwrite(data, 1, len, f);
  fclose(f);

  if (!append && len == 0) {
    ESP_LOGI(TAG, "File cleared: %s", path);
    return ESP_OK;
  }
  return bytes_written;
}