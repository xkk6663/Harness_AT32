#include "ota_upgrade_state.h"
#include "ota_flash_store.h"

UpgradeState UPGRADE_GetState(void)
{
    return (UpgradeState)FlashStore_Read(UPGRADE_STATE_ADDR, STATE_RUNNING);
}

void UPGRADE_SetState(UpgradeState state)
{
    FlashStore_Write(UPGRADE_STATE_ADDR, (uint32_t)state);
}

uint8_t UPGRADE_IsState(UpgradeState state)
{
    return (UPGRADE_GetState() == state) ? 1 : 0;
}
