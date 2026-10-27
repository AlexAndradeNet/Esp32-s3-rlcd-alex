#pragma once

#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <driver/sdmmc_host.h>

class CustomSDPort {
private:
  const char *TAG = "SDPort";
  const char *SdName_;
  int is_SdcardInitOK = 0;
  sdmmc_card_t *sdcard_host = NULL;
  sdmmc_host_t host_;
  sdmmc_slot_config_t slot_config_;
  esp_vfs_fat_sdmmc_mount_config_t mount_config_;
  bool Mount();
public:
  CustomSDPort(const char *SdName, int clk = 38, int cmd = 21, int d0 = 39, int width = 1);
  ~CustomSDPort();

  int SDPort_WriteFile(const char *path, const void *data, size_t data_len);
  int SDPort_ReadFile(const char *path, uint8_t *buffer, size_t *outLen);
  int SDPort_ReadOffset(const char *path, void *buffer, size_t len, size_t offset);
  int SDPort_WriteOffset(const char *path, const void *data, size_t len, bool append);
  // Probes the card and remounts it if it stopped answering (or always, with forceRemount).
  // Never call it while a file is open.
  bool SDPort_EnsureReady(bool forceRemount = false);
};