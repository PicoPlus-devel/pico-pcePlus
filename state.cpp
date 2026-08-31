#include "state.h"
#include "FrensHelpers.h"
#include "ff.h"
#include <string.h>

extern "C"
{
#include "pce-go.h"
#include "pce.h"
#include "gfx.h"
#include "cd.h"
}

int Emulator_SaveState(const char *path)
{
    // FatFS is not reentrant, and for CD games core1 runs cd_audio_update()
    // (f_read on the audio track) continuously in the video background task.
    // Writing ~400 KB here without serialising corrupts the transfer.
    cd_sd_lock();
    FIL *fil = (FIL *)Frens::f_malloc(sizeof(FIL));
    FRESULT fr = f_open(fil, path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK)
    {
        printf("Cannot open save state file: %d (%s)\n", fr, path);
        Frens::f_free(fil);
        cd_sd_unlock();
        return -1;
    }

    UINT bw;
    f_write(fil, SAVESTATE_HEADER, 8, &bw);

    // Walk every applicable list. This previously wrote only SaveStateVars, so
    // device-written states omitted the SuperGrafx state (VRAM2/SPRAM2/VDC2/VPC)
    // and the CD RAM the game actually executes from. Both are now included,
    // matching pce-go's own SaveState().
    save_var_t *lists[] = {
        SaveStateVars,
        PCE.VPC.is_sgx ? SgxSaveStateVars : nullptr,
        CD.cd_attached ? CdSaveStateVars : nullptr,
    };
    for (size_t li = 0; li < sizeof(lists) / sizeof(lists[0]); li++)
    {
        if (!lists[li]) continue;
        for (save_var_t *var = lists[li]; var->ptr; var++)
        {
            void *ptr = var->desc.type == 5 ? *((void **)var->ptr) : var->ptr;
            size_t len = var->desc.len;
            if (!ptr) continue;
            f_write(fil, &var->desc, sizeof(var->desc), &bw);
            f_write(fil, ptr, len, &bw);
        }
    }

    FRESULT frc = f_close(fil);
    Frens::f_free(fil);

    if (frc != FR_OK)
    {
        printf("Error closing save state file: %d (%s)\n", frc, path);
        cd_sd_unlock();
        return -1;
    }
    cd_sd_unlock();
    return 0;
}

int Emulator_LoadState(const char *path)
{
    // Same reentrancy hazard as the save path. Symptom of the race was a state
    // that reloaded with correct background tiles (low VRAM) but garbage
    // sprites (high VRAM) — a partially clobbered read.
    cd_sd_lock();
    FIL *fil = (FIL *)Frens::f_malloc(sizeof(FIL));
    FRESULT fr = f_open(fil, path, FA_READ);
    if (fr != FR_OK)
    {
        printf("Cannot open load state file: %d (%s)\n", fr, path);
        Frens::f_free(fil);
        cd_sd_unlock();
        return -1;
    }

    char header[8];
    UINT br;
    f_read(fil, header, 8, &br);
    if (br != 8 || memcmp(header, SAVESTATE_HEADER, 8) != 0)
    {
        printf("Save state header mismatch (%s)\n", path);
        f_close(fil);
        Frens::f_free(fil);
        cd_sd_unlock();
        return -1;
    }

    block_hdr_t block;
    while (f_read(fil, &block, sizeof(block), &br) == FR_OK && br == sizeof(block))
    {
        FSIZE_t block_end = f_tell(fil) + block.len;

        save_var_t *lists[] = { SaveStateVars, SgxSaveStateVars, CdSaveStateVars };
        bool matched = false;
        for (size_t li = 0; li < sizeof(lists) / sizeof(lists[0]) && !matched; li++)
        {
            for (save_var_t *var = lists[li]; var->ptr; var++)
            {
                if (strncmp(var->desc.key, block.key, 12) != 0)
                    continue;
                void *ptr = var->desc.type == 5 ? *((void **)var->ptr) : var->ptr;
                if (!ptr) break;   // list present but buffer not allocated
                size_t len = var->desc.len < (size_t)block.len ? var->desc.len : (size_t)block.len;
                f_read(fil, ptr, len, &br);
                if (len < var->desc.len)
                    memset((uint8_t *)ptr + len, 0, var->desc.len - len);
                matched = true;
                break;
            }
        }
        f_lseek(fil, block_end);
    }

    FRESULT frc = f_close(fil);
    Frens::f_free(fil);

    if (frc != FR_OK)
    {
        printf("Error closing load state file: %d (%s)\n", frc, path);
        cd_sd_unlock();
        return -1;
    }

    for (int i = 0; i < 8; i++)
        pce_bank_set(i, PCE.MMR[i]);

    gfx_reset(true);
    PCE.VDC.mode_chg = 1;

    cd_sd_unlock();

    return 0;
}
