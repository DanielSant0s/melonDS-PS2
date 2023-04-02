/*
    Copyright 2018 Hydr8gon

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <algorithm>
#include <chrono>
#include <dirent.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <gsKit.h>
#include <libpad.h>
#include <vector>
#include <kernel.h>
#include <string>
#include <math.h>

#include <sifrpc.h>
#include <loadfile.h>
#include <libmc.h>
#include <iopheap.h>
#include <iopcontrol.h>
#include <smod.h>
#include <audsrv.h>
#include <sys/stat.h>

#include <sbv_patches.h>
#include <smem.h>

#include <unistd.h>
#include <sys/fcntl.h>
#include <tamtypes.h>

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <fileio.h>

// Deal with conflicting typedefs
#define u64 u64_
#define s64 s64_

#define SCREEN_W 640.0f
#define SCREEN_H 448.0f

#include "../Config.h"
#include "../Savestate.h"
#include "../GPU.h"
#include "../NDS.h"
#include "../SPU.h"
#include "../version.h"

#define lerp(value, from_max, to_max) ((((value*10) * (to_max*10))/(from_max*10))/10)

using namespace std;

extern void *_gp;

extern unsigned char sio2man_irx;
extern unsigned int size_sio2man_irx;
extern unsigned char mcman_irx;
extern unsigned int size_mcman_irx;
extern unsigned char mcserv_irx;
extern unsigned int size_mcserv_irx;
extern unsigned char padman_irx;
extern unsigned int size_padman_irx;
extern unsigned char libsd_irx;
extern unsigned int size_libsd_irx;
extern unsigned char usbd_irx;
extern unsigned int size_usbd_irx;
extern unsigned char bdm_irx;
extern unsigned int size_bdm_irx;
extern unsigned char bdmfs_vfat_irx;
extern unsigned int size_bdmfs_vfat_irx;
extern unsigned char usbmass_bd_irx;
extern unsigned int size_usbmass_bd_irx;
extern unsigned char audsrv_irx;
extern unsigned int size_audsrv_irx;


static const u64 BLACK_RGBAQ   = GS_SETREG_RGBAQ(0x00,0x00,0x00,0x80,0x00);

GSGLOBAL *gsGlobal = NULL;
GSFONTM *font = NULL;
static int vsync_sema_id = 0;

GSTEXTURE *vram_buffer;
struct padButtonStatus padbuttons;
uint32_t pad = 0;
uint32_t oldpad = 0;

int _newlib_heap_size_user = 330 * 1024 * 1024;
int screen_x[2], screen_y[2];
float screen_scale;
void (*drawFunc)();

vector<const char*> OptionDisplay =
{
    "Boot game directly",
    "Threaded 3D renderer",
    "Separate savefiles",
    "Screen layout"
};

vector<vector<const char*>> OptionValuesDisplay =
{
    { "Off", "On " },
    { "Off", "On " },
    { "Off", "On " },
    { "Standard", "Side by Side" }
};

vector<int*> OptionValues =
{
    &Config::DirectBoot,
    &Config::Threaded3D,
    &Config::SavestateRelocSRAM,
    &Config::ScreenLayout
};

u8 *BufferData[2];
uint8_t AudioIdx = 0;

u32 *Framebuffer;
unsigned int TouchBoundLeft, TouchBoundRight, TouchBoundTop, TouchBoundBottom;

s32 EmuSema;


static char padBuf[256] __attribute__((aligned(64)));

static char actAlign[6];
static int actuators;

int port, slot;

int waitPadReady(int port, int slot)
{
    int state;
    int lastState;
    char stateString[16];

    state = padGetState(port, slot);
    lastState = -1;
    while((state != PAD_STATE_STABLE) && (state != PAD_STATE_FINDCTP1)) {
        if (state != lastState) {
            padStateInt2String(state, stateString);
            printf("Please wait, pad(%d,%d) is in state %s\n",
                       port, slot, stateString);
        }
        lastState = state;
        state=padGetState(port, slot);
    }
    // Were the pad ever 'out of sync'?
    if (lastState != -1) {
        printf("Pad OK!\n");
    }
    return 0;
}


/*
 * initializePad()
 */
int initializePad(int port, int slot)
{

    int ret;
    int modes;
    int i;

    waitPadReady(port, slot);

    // How many different modes can this device operate in?
    // i.e. get # entrys in the modetable
    modes = padInfoMode(port, slot, PAD_MODETABLE, -1);
    printf("The device has %d modes\n", modes);

    if (modes > 0) {
        printf("( ");
        for (i = 0; i < modes; i++) {
            printf("%d ", padInfoMode(port, slot, PAD_MODETABLE, i));
        }
        printf(")");
    }

    printf("It is currently using mode %d\n",
               padInfoMode(port, slot, PAD_MODECURID, 0));

    // If modes == 0, this is not a Dual shock controller
    // (it has no actuator engines)
    if (modes == 0) {
        printf("This is a digital controller?\n");
        return 1;
    }

    // Verify that the controller has a DUAL SHOCK mode
    i = 0;
    do {
        if (padInfoMode(port, slot, PAD_MODETABLE, i) == PAD_TYPE_DUALSHOCK)
            break;
        i++;
    } while (i < modes);
    if (i >= modes) {
        printf("This is no Dual Shock controller\n");
        return 1;
    }

    // If ExId != 0x0 => This controller has actuator engines
    // This check should always pass if the Dual Shock test above passed
    ret = padInfoMode(port, slot, PAD_MODECUREXID, 0);
    if (ret == 0) {
        printf("This is no Dual Shock controller??\n");
        return 1;
    }

    printf("Enabling dual shock functions\n");

    // When using MMODE_LOCK, user cant change mode with Select button
    padSetMainMode(port, slot, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);

    waitPadReady(port, slot);
    printf("infoPressMode: %d\n", padInfoPressMode(port, slot));

    waitPadReady(port, slot);
    printf("enterPressMode: %d\n", padEnterPressMode(port, slot));

    waitPadReady(port, slot);
    actuators = padInfoAct(port, slot, -1, 0);
    printf("# of actuators: %d\n",actuators);

    if (actuators != 0) {
        actAlign[0] = 0;   // Enable small engine
        actAlign[1] = 1;   // Enable big engine
        actAlign[2] = 0xff;
        actAlign[3] = 0xff;
        actAlign[4] = 0xff;
        actAlign[5] = 0xff;

        waitPadReady(port, slot);
        printf("padSetActAlign: %d\n",
                   padSetActAlign(port, slot, actAlign));
    }
    else {
        printf("Did not find any actuators.\n");
    }

    waitPadReady(port, slot);

    return 1;
}

struct padButtonStatus readPad(int port, int slot)
{
    struct padButtonStatus buttons;
    int ret;    

    do {
    	ret = padGetState(port, slot);
    } while((ret != PAD_STATE_STABLE) && (ret != PAD_STATE_FINDCTP1));  

    ret = padRead(port, slot, &buttons);      

    return buttons;

}

void pad_init()
{
    int ret;

    padInit(0);

    port = 0; // 0 -> Connector 1, 1 -> Connector 2
    slot = 0; // Always zero if not using multitap

    printf("PortMax: %d\n", padGetPortMax());
    printf("SlotMax: %d\n", padGetSlotMax(port));


    if((ret = padPortOpen(port, slot, padBuf)) == 0) {
        printf("padOpenPort failed: %d\n", ret);
        SleepThread();
    }

    if(!initializePad(port, slot)) {
        printf("pad initalization failed!\n");
        SleepThread();
    }
}

void initMC(void)
{
   int ret;
   // mc variables
   int mc_Type, mc_Free, mc_Format;

   
   printf("initMC: Initializing Memory Card\n");

   ret = mcInit(MC_TYPE_XMC);
   
   if( ret < 0 ) {
	printf("initMC: failed to initialize memcard server.\n");
   } else {
       printf("initMC: memcard server started successfully.\n");
   }
   
   // Since this is the first call, -1 should be returned.
   // makes me sure that next ones will work !
   mcGetInfo(0, 0, &mc_Type, &mc_Free, &mc_Format); 
   mcSync(MC_WAIT, NULL, &ret);
}

bool checkPressed(uint32_t button){
    pad = 0xffff ^ padbuttons.btns;
    return (pad & button) && (!(oldpad & button));
}

bool checkReleased(uint32_t button){
    pad = 0xffff ^ padbuttons.btns;
    return (oldpad & button) && (!(pad & button));
}

int y_printf = 60;
void scr_printf(unsigned int color,  const char *format, ...) {
    __gnuc_va_list arg;
    int done;
    va_start(arg, format);
    char msg[512];
    done = vsprintf(msg, format, arg);
    va_end(arg);
    gsKit_fontm_print_scaled(gsGlobal, font, 5-0.5f, y_printf-0.5f, 1, 0.6, color, msg);
    y_printf += 20;
}

/* PRIVATE METHODS */
static int vsync_handler()
{
   iSignalSema(vsync_sema_id);

   ExitHandler();
   return 0;
}

/* Copy of gsKit_sync_flip, but without the 'flip' */
static void gsKit_sync(GSGLOBAL *gsGlobal)
{
   if (!gsGlobal->FirstFrame) WaitSema(vsync_sema_id);
   while (PollSema(vsync_sema_id) >= 0)
   	;
}

/* Copy of gsKit_sync_flip, but without the 'sync' */
static void gsKit_flip(GSGLOBAL *gsGlobal)
{
   if (!gsGlobal->FirstFrame)
   {
      
	  if (gsGlobal->DoubleBuffering == GS_SETTING_ON)
      {
         GS_SET_DISPFB2( gsGlobal->ScreenBuffer[
               gsGlobal->ActiveBuffer & 1] / 8192,
               gsGlobal->Width / 64, gsGlobal->PSM, 0, 0 );

         gsGlobal->ActiveBuffer ^= 1;
      }

   }

   gsKit_setactive(gsGlobal);
}

void flipScreen()
{	
	//gsKit_set_finish(gsGlobal);
	gsKit_queue_exec(gsGlobal);
	gsKit_finish();
	gsKit_flip(gsGlobal);
	gsKit_TexManager_nextFrame(gsGlobal);
}


string Menu()
{
	char path[256];
	getcwd(path, 256);
    string rompath = strcat(path, "roms/");
    bool options = false;

    while (rompath.find(".nds", (rompath.length() - 4)) == string::npos)
    {
        
        unsigned int selection = 0;
        vector<string> files;

        DIR* dir = opendir(rompath.c_str());
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL)
        {
            string name = entry->d_name;
            if (S_ISDIR(entry->d_stat.st_mode) || name.find(".nds", (name.length() - 4)) != string::npos)
                files.push_back(name);
        }
        closedir(dir);
        sort(files.begin(), files.end());

        while (true)
        {
            y_printf = 60;
            gsKit_clear(gsGlobal, BLACK_RGBAQ);	
            gsKit_fontm_print_scaled(gsGlobal, font, 5, 20, 1, 0.6, 0x80FFFFFF, "melonDS " MELONDS_VERSION);
            padbuttons = readPad(0, 0);

            if (options)
            {
                if (checkPressed(PAD_CROSS))
                {
                    (*OptionValues[selection])++;
                    if (*OptionValues[selection] >= (int)OptionValuesDisplay[selection].size())
                        *OptionValues[selection] = 0;
                }
                else if (checkPressed(PAD_UP) && selection > 0)
                {
                    selection--;
                }
                else if (checkPressed(PAD_DOWN) && selection < OptionDisplay.size() - 1)
                {
                    selection++;
                }
                else if (checkPressed(PAD_SQUARE))
                {
                    Config::Save();
                    options = false;
                    break;
                }

                for (unsigned int i = 0; i < OptionDisplay.size(); i++)
                {
                    if (i == selection)
                    {
                        scr_printf(0x8000FFFF, "%s: %s", OptionDisplay[i], OptionValuesDisplay[i][*OptionValues[i]]);
                    }
                    else
                    {
                        scr_printf(0x80FFFFFF, "%s: %s", OptionDisplay[i], OptionValuesDisplay[i][*OptionValues[i]]);
                    }
                }
                scr_printf(0x80FFFFFF, "");
                scr_printf(0x80FFFFFF, "Press Square to return to the file browser.");
            }
            else
            {
                if (checkPressed(PAD_CROSS) && files.size() > 0)
                {
                    rompath += "/" + files[selection];
                    break;
                }
                else if (checkPressed(PAD_CIRCLE) && rompath != "mass:/melonDS/")
                {
                    rompath = rompath.substr(0, rompath.rfind("/"));
                    break;
                }
                else if (checkPressed(PAD_UP) && selection > 0)
                {
                    selection--;
                }
                else if (checkPressed(PAD_DOWN) && selection < files.size() - 1)
                {
                    selection++;
                }
                else if (checkPressed(PAD_SQUARE))
                {
                    Config::Load();
                    options = true;
                    break;
                }

                for (unsigned int i = 0; i < files.size(); i++)
                {
                    if (i == selection)
                        scr_printf(0x8000FFFF, "%s", files[i].c_str());
                    else
                        scr_printf(0x80FFFFFF, "%s", files[i].c_str());
                }
                
                scr_printf(0x80FFFFFF, "");
                scr_printf(0x80FFFFFF, "Press Square to open the options menu.");
            }

            oldpad = pad;
            flipScreen();
            
        }
    }
    
    flipScreen();

    return rompath;
}

void drawSingle(){
    gsKit_TexManager_bind(gsGlobal, vram_buffer);
	gsKit_prim_sprite_texture(gsGlobal, vram_buffer, 
                                320-vram_buffer->Width/2, 224-vram_buffer->Height/2, 
                                0.0f, 0.0f, 
                                320+vram_buffer->Width/2, 
                                224+vram_buffer->Height/2, 
                                vram_buffer->Width,
                                vram_buffer->Height, 
                                1, 0x80808080);
}

void drawDouble(){
    gsKit_TexManager_bind(gsGlobal, vram_buffer);
	gsKit_prim_sprite_texture(gsGlobal, vram_buffer, 
                                screen_x[0], screen_y[0], 
                                0.0f, 0.0f, 
                                vram_buffer->Width+screen_x[0], 
                                192.0f+screen_y[0], 
                                vram_buffer->Width, 192.0f, 
                                1, 0x80808080);

	gsKit_prim_sprite_texture(gsGlobal, vram_buffer, 
                                screen_x[1], screen_y[1], 
                                0.0f, vram_buffer->Height/2, 
                                vram_buffer->Width+screen_x[1], 
                                192.0f+screen_y[1], 
                                vram_buffer->Width, vram_buffer->Height, 
                                1, 0x80808080);
}

void SetScreenLayout()
{
    switch (Config::ScreenLayout){
        case 0:
            drawFunc = drawSingle;
            screen_scale = SCREEN_H / (192 * 2);
            screen_x[0] = (SCREEN_W - 256 * screen_scale) / 2;
            screen_y[0] = 0;
            TouchBoundLeft = screen_x[0];
            TouchBoundRight = TouchBoundLeft + 256 * screen_scale;
            TouchBoundTop = SCREEN_H / 2;
            TouchBoundBottom = SCREEN_H;
            break;
        case 1:
            drawFunc = drawDouble;
            screen_scale = SCREEN_W / (256 * 2);
            screen_x[0] = 64;
            screen_x[1] = 256 * screen_scale;
            screen_y[0] = screen_y[1] = (SCREEN_H - 192 * screen_scale) / 2;
            TouchBoundLeft = screen_x[1];
            TouchBoundRight = TouchBoundLeft + 256 * screen_scale;
            TouchBoundTop = screen_y[0];
            TouchBoundBottom = TouchBoundTop + 192 * screen_scale;
            break;
        default:
            break;
    }
}

int AdvFrame(unsigned int argc, void *args)
{
    while (true)
    {
        //chrono::steady_clock::time_point start = chrono::steady_clock::now();

        //WaitSema(EmuSema);
        NDS::RunFrame();
        printf("RunTestz\n");
        //SignalSema(EmuSema);
        memcpy(vram_buffer->Mem, GPU::Framebuffer, 256 * 384 * 4);
        gsKit_TexManager_invalidate(gsGlobal, vram_buffer);
        gsKit_TexManager_bind(gsGlobal, vram_buffer);
        

        //while (chrono::duration_cast<chrono::duration<double>>(chrono::steady_clock::now() - start).count() < (float)1 / 60);
    }
    return 0;
}

#define ORIG_SAMPLES               700
#define TARGET_SAMPLES            1024
#define ORIG_SRATE      32823.6328125f
#define TARGET_SRATE             48000

void FillAudioBuffer()
{
    s16 buf_in[ORIG_SAMPLES * 2];
    s16 *buf_out = (s16*)BufferData[AudioIdx];

    int num_in = SPU::ReadOutput(buf_in, ORIG_SAMPLES);
    int num_out = TARGET_SAMPLES;

    int margin = 6;
    if (num_in < ORIG_SAMPLES - margin)
    {
        int last = num_in - 1;
        if (last < 0)
            last = 0;

        for (int i = num_in; i < ORIG_SAMPLES - margin; i++)
            ((u32*)buf_in)[i] = ((u32*)buf_in)[last];

        num_in = ORIG_SAMPLES - margin;
    }

    float res_incr = (float)num_in / num_out;
    float res_timer = 0;
    int res_pos = 0;

    for (int i = 0; i < TARGET_SAMPLES; i++)
    {
        buf_out[i * 2] = buf_in[res_pos * 2];
        buf_out[i * 2 + 1] = buf_in[res_pos * 2 + 1];

        res_timer += res_incr;
        while (res_timer >= 1)
        {
            res_timer--;
            res_pos++;
        }
    }
}

int PlayAudio(unsigned int argc, void *argv)
{

    while (true)
    {
        FillAudioBuffer();
        audsrv_play_audio((const char*)BufferData[AudioIdx], TARGET_SAMPLES);
        AudioIdx = (AudioIdx + 1) % 2;
    }
    return 0;
}

void initGraphics()
{
	ee_sema_t sema;
    sema.init_count = 0;
    sema.max_count = 1;
    sema.option = 0;
    vsync_sema_id = CreateSema(&sema);

	gsGlobal = gsKit_init_global();
    gsKit_TexManager_setmode(gsGlobal, ETM_DIRECT);

	gsGlobal->Mode = gsKit_check_rom();
	if (gsGlobal->Mode == GS_MODE_PAL){
		gsGlobal->Height = 512;
	} else {
		gsGlobal->Height = 448;
	}

	gsGlobal->PSM  = GS_PSM_CT32;
	gsGlobal->PSMZ = GS_PSMZ_16S;
	gsGlobal->ZBuffering = GS_SETTING_OFF;
	gsGlobal->DoubleBuffering = GS_SETTING_ON;
	gsGlobal->PrimAlphaEnable = GS_SETTING_ON;
	gsGlobal->Dithering = GS_SETTING_OFF;
	
	gsGlobal->Interlace = GS_INTERLACED;
	gsGlobal->Field = GS_FIELD;
	
	gsKit_set_primalpha(gsGlobal, GS_SETREG_ALPHA(0, 1, 0, 1, 128), 0);

	dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
	dmaKit_chan_init(DMA_CHANNEL_GIF);

	printf("\nGraphics: created %ix%i video surface\n",
		gsGlobal->Width, gsGlobal->Height);

	gsKit_set_clamp(gsGlobal, GS_CMODE_REPEAT);
	gsKit_vram_clear(gsGlobal);
	gsKit_init_screen(gsGlobal);
	gsKit_set_display_offset(gsGlobal, -0.5f, -0.5f);
	gsKit_TexManager_init(gsGlobal);
    gsKit_add_vsync_handler(vsync_handler);
	gsKit_mode_switch(gsGlobal, GS_ONESHOT);
	
	gsKit_sync_flip(gsGlobal);

    gsKit_clear(gsGlobal, BLACK_RGBAQ);	
	gsKit_vsync_wait();
	flipScreen();
	gsKit_clear(gsGlobal, BLACK_RGBAQ);	
	gsKit_vsync_wait();
	flipScreen();

}



int main(int argc, char **argv){

    SifInitRpc(0);
    while (!SifIopReset("", 0)){};
    while (!SifIopSync()){};
    SifInitRpc(0);
	int x, y;
	int cy;
	u8  *image;
	u8  *p;
    
    // install sbv patch fix
    printf("Installing SBV Patches...\n");
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check(); 
    sbv_patch_fileio(); 

    SifExecModuleBuffer(&sio2man_irx, size_sio2man_irx, 0, NULL, NULL);
    SifExecModuleBuffer(&mcman_irx, size_mcman_irx, 0, NULL, NULL);
    SifExecModuleBuffer(&mcserv_irx, size_mcserv_irx, 0, NULL, NULL);
    initMC();

    SifExecModuleBuffer(&padman_irx, size_padman_irx, 0, NULL, NULL);
    SifExecModuleBuffer(&libsd_irx, size_libsd_irx, 0, NULL, NULL);

    // load pad & mc modules 
    printf("Installing Pad & MC modules...\n");

    // load USB modules    
    SifExecModuleBuffer(&usbd_irx, size_usbd_irx, 0, NULL, NULL);
    SifExecModuleBuffer(&bdm_irx, size_bdm_irx, 0, NULL, NULL);
    SifExecModuleBuffer(&bdmfs_vfat_irx, size_bdmfs_vfat_irx, 0, NULL, NULL);
    SifExecModuleBuffer(&usbmass_bd_irx, size_usbmass_bd_irx, 0, NULL, NULL);

    SifExecModuleBuffer(&audsrv_irx, size_audsrv_irx, 0, NULL, NULL);
    audsrv_init();
    audsrv_set_volume(MAX_VOLUME);

    struct audsrv_fmt_t format;
    format.bits = 16;
	format.freq = 24000;
	format.channels = 2;
	audsrv_set_format(&format);

    //waitUntilDeviceIsReady by fjtrujy

    struct stat buffer;
    int ret = -1;
    int retries = 50;

    while(ret != 0 && retries > 0)
    {
        ret = stat("mass:/", &buffer);
        /* Wait until the device is ready */
        nopdelay();

        retries--;
    }

    pad_init();

    initGraphics();
    
    font = gsKit_init_fontm();
	gsKit_fontm_upload(gsGlobal, font);
	font->Spacing = 0.70f;

    ee_sema_t sema_params;
    sema_params.max_count = 1;
    sema_params.init_count = 1;
    EmuSema = CreateSema(&sema_params);

    string rompath = Menu();
    string srampath = rompath.substr(0, rompath.rfind(".")) + ".sav";
    string statepath = rompath.substr(0, rompath.rfind(".")) + ".mln";

    Config::Load();
    if (!Config::HasConfigFile("bios7.bin") || !Config::HasConfigFile("bios9.bin") || !Config::HasConfigFile("firmware.bin"))
    {
        while (true){
            gsKit_clear(gsGlobal, BLACK_RGBAQ);	
            gsKit_fontm_print_scaled(gsGlobal, font, 5-0.5f, 20-0.5f, 1, 0.6, 0x80FFFFFF, "One or more of the following required files don't exist:");
            gsKit_fontm_print_scaled(gsGlobal, font, 5-0.5f, 40-0.5f, 1, 0.6, 0x80FFFFFF, "bios7.bin -- ARM7 BIOS");
            gsKit_fontm_print_scaled(gsGlobal, font, 5-0.5f, 60-0.5f, 1, 0.6, 0x80FFFFFF, "bios9.bin -- ARM9 BIOS");
            gsKit_fontm_print_scaled(gsGlobal, font, 5-0.5f, 80-0.5f, 1, 0.6, 0x80FFFFFF, "firmware.bin -- firmware image");
            gsKit_fontm_print_scaled(gsGlobal, font, 5-0.5f, 150-0.5f, 1, 0.6, 0x80FFFFFF, "Dump the files from your DS and place them in your pendrive.");
            flipScreen();
        }
    }

    NDS::Init();
    if (!NDS::LoadROM(rompath.c_str(), srampath.c_str(), Config::DirectBoot))
    {
        while (true){
            gsKit_clear(gsGlobal, BLACK_RGBAQ);	
            gsKit_fontm_print_scaled(gsGlobal, font, 5-0.5f, 20-0.5f, 1, 0.6, 0x80FFFFFF, "Failed to load ROM. Make sure the file can be accessed.");
            flipScreen();
        }
    }

    //sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);

    SetScreenLayout();

    ee_thread_t thread_main;
	
	thread_main.gp_reg = &_gp;
    thread_main.func = (void*)AdvFrame;
    thread_main.stack = malloc(0x10000);
    thread_main.stack_size = 0x10000;
    thread_main.initial_priority = 0x40;
	int main = CreateThread(&thread_main);
    //StartThread(main, NULL);

    ee_thread_t thread_audio;
	
	thread_audio.gp_reg = &_gp;
    thread_audio.func = (void*)PlayAudio;
    thread_audio.stack = malloc(0x10000);
    thread_audio.stack_size = 0x10000;
    thread_audio.initial_priority = 0x40;
	int audio_thd = CreateThread(&thread_audio);
    //StartThread(audio_thd, NULL);

    BufferData[0] = (u8*)malloc(4096);
    BufferData[1] = (u8*)malloc(4096);

    vram_buffer = (GSTEXTURE*)malloc(sizeof(GSTEXTURE));

    vram_buffer->Width = 256;
    vram_buffer->Height = 384;
    vram_buffer->PSM = GS_PSM_CT32;
    vram_buffer->Filter = GS_FILTER_NEAREST;
    vram_buffer->Mem = GPU::Framebuffer;

    uint32_t keys[] = { PAD_CROSS, PAD_CIRCLE, PAD_SELECT, PAD_START, PAD_RIGHT, PAD_LEFT, PAD_UP, PAD_DOWN, PAD_R1, PAD_L1, PAD_SQUARE, PAD_TRIANGLE };
    bool Touching = false;
    
    while (true)
    {

        padbuttons = readPad(0, 0);

        if (checkPressed(PAD_L1) || checkPressed(PAD_R1))
        {
            Savestate* state = new Savestate(const_cast<char*>(statepath.c_str()), checkPressed(PAD_L1));
            if (!state->Error)
            {
                WaitSema(EmuSema);
                NDS::DoSavestate(state);
                SignalSema(EmuSema);
            }
            delete state;
        }

        for (int i = 0; i < 12; i++)
        {
            if (checkPressed(keys[i]))
                NDS::PressKey(i > 9 ? i + 6 : i);
            else if (checkReleased(keys[i]))
                NDS::ReleaseKey(i > 9 ? i + 6 : i);
        }
        
        /*SceTouchData touch;
        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);

        if (touch.reportNum > 0)
        {
            
            int touch_x = lerp(touch.report[0].x, 1920, 960);
            int touch_y = lerp(touch.report[0].y, 1088, 544);
            
            if (touch_x > TouchBoundLeft && touch_x < TouchBoundRight && touch_y > TouchBoundTop && touch_y < TouchBoundBottom)
            {
                int x, y;
                switch (Config::ScreenLayout){
                case 0:
                case 1:
                    x = (touch_x - TouchBoundLeft) / screen_scale;
                    y = (touch_y - TouchBoundTop) / screen_scale;
                    break;
                case 2:
                    x = (touch_y - TouchBoundLeft) / screen_scale;
                    y = (touch_x - TouchBoundTop) / screen_scale;
                    break;
                default:
                    break;
                }
                if (!Touching) NDS::PressKey(16 + 6);
                NDS::TouchScreen(x, y);
                Touching = true;
            }else if (Touching)
            {
                NDS::ReleaseKey(16 + 6);
                NDS::ReleaseScreen();
                Touching = false;
            }
        }
        else if (Touching)
        {
            NDS::ReleaseKey(16 + 6);
            NDS::ReleaseScreen();
            Touching = false;
        }*/

        NDS::ReleaseKey(16 + 6);
        NDS::ReleaseScreen();

        WaitSema(EmuSema);
        NDS::RunFrame();
        SignalSema(EmuSema);
        gsKit_TexManager_invalidate(gsGlobal, vram_buffer);
        gsKit_clear(gsGlobal, 0x80000000);	
        drawFunc();
        flipScreen();
        
        oldpad = pad;
        
    }

    NDS::DeInit();
    gsKit_deinit_global(gsGlobal);
    return 0;
}