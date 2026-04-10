#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <3ds.h>
#include <citro2d.h>
#include <tremor/ivorbisfile.h>
#include <tremor/ivorbiscodec.h>

#include <assert.h>
#include <time.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define SCREEN_WIDTH  400
#define SCREEN_HEIGHT 240
#define MAX_SPRITES 50

// Simple sprite struct
typedef struct
{
	C2D_Sprite spr;
	int time; // time for sprite to stay on screen
} Sprite;

static C2D_SpriteSheet spriteSheet;
static Sprite sprites[MAX_SPRITES];

C2D_TextBuf g_staticBuf;
C2D_Text g_staticText[11];
C2D_Font font[2];

static const char *PATH = "romfs:/mus_story.ogg";  // Path to Ogg Vorbis file to play

static const int THREAD_AFFINITY = -1;           // Execute thread on any core
static const int THREAD_STACK_SZ = 32 * 1024;    // 32kB stack for audio thread

ndspWaveBuf s_waveBufs[6];
int16_t *s_audioBuffer = NULL;

LightEvent s_event;
volatile bool s_quit = false;  // Quit flag

// Retrieve strings for libvorbisidec errors
const char *vorbisStrError(int error)
{
    switch(error) {
        case OV_FALSE:
            return "OV_FALSE: A request did not succeed.";
        case OV_HOLE:
            return "OV_HOLE: There was a hole in the page sequence numbers.";
        case OV_EREAD:
            return "OV_EREAD: An underlying read, seek or tell operation "
                   "failed.";
        case OV_EFAULT:
            return "OV_EFAULT: A NULL pointer was passed where none was "
                   "expected, or an internal library error was encountered.";
        case OV_EIMPL:
            return "OV_EIMPL: The stream used a feature which is not "
                   "implemented.";
        case OV_EINVAL:
            return "OV_EINVAL: One or more parameters to a function were "
                   "invalid.";
        case OV_ENOTVORBIS:
            return "OV_ENOTVORBIS: This is not a valid Ogg Vorbis stream.";
        case OV_EBADHEADER:
            return "OV_EBADHEADER: A required header packet was not properly "
                   "formatted.";
        case OV_EVERSION:
            return "OV_EVERSION: The ID header contained an unrecognised "
                   "version number.";
        case OV_EBADPACKET:
            return "OV_EBADPACKET: An audio packet failed to decode properly.";
        case OV_EBADLINK:
            return "OV_EBADLINK: We failed to find data we had seen before or "
                   "the stream was sufficiently corrupt that seeking is "
                   "impossible.";
        case OV_ENOSEEK:
            return "OV_ENOSEEK: An operation that requires seeking was "
                   "requested on an unseekable stream.";
        default:
            return "Unknown error.";
    }
}

static void initSprites() {
	for (size_t i = 0; i < 11; i++){
		Sprite* sprite = &sprites[i];
		C2D_SpriteFromSheet(&sprite->spr, spriteSheet, i);
		C2D_SpriteSetCenter(&sprite->spr, 0.5f, 0.5f);
		C2D_SpriteSetPos(&sprite->spr, SCREEN_WIDTH/2, SCREEN_HEIGHT/2);
		sprite->time = 2;
	}
}

static void initText() {
	g_staticBuf  = C2D_TextBufNew(4096); // support up to 4096 glyphs in the buffer
	font[0] = C2D_FontLoad("romfs:/DTM-Mono.bcfnt");
	font[1] = C2D_FontLoad("romfs:/DTM-Sans.bcfnt");

	C2D_TextFontParse(&g_staticText[0], font[0], g_staticBuf, "Long ago, two races\nruled over Earth:\nHUMANS and MONSTERS.");
	C2D_TextFontParse(&g_staticText[1], font[0], g_staticBuf, "One day, war broke\nout between the two\nraces.");
	C2D_TextFontParse(&g_staticText[2], font[0], g_staticBuf, "After a long battle,\nthe humans were\nvictorious.");
	C2D_TextFontParse(&g_staticText[3], font[0], g_staticBuf, "They sealed the monsters\nunderground with a magic\nspell.");
	C2D_TextFontParse(&g_staticText[4], font[0], g_staticBuf, "Many years later...");
	C2D_TextFontParse(&g_staticText[5], font[0], g_staticBuf, "MT. EBOTT\n201X");
	C2D_TextFontParse(&g_staticText[6], font[0], g_staticBuf, "Legends say that those\nwho climb the mountain\nnever return.");
	C2D_TextFontParse(&g_staticText[7], font[0], g_staticBuf, "");
	C2D_TextFontParse(&g_staticText[8], font[0], g_staticBuf, "");
	C2D_TextFontParse(&g_staticText[9], font[0], g_staticBuf, "");
	C2D_TextFontParse(&g_staticText[10], font[0], g_staticBuf, "");

	// C2D_TextOptimize(&g_staticText[0]);
	// C2D_TextOptimize(&g_staticText[1]);
	for(size_t i = 0; i < 11; i++){
		C2D_TextOptimize(&g_staticText[i]);
	}
}

bool audioInit(OggVorbis_File *vorbisFile_) {
    vorbis_info *vi = ov_info(vorbisFile_, -1);

    // Setup NDSP
    ndspChnReset(0);
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
    ndspChnSetRate(0, vi->rate);
    ndspChnSetFormat(0, vi->channels == 1
        ? NDSP_FORMAT_MONO_PCM16
        : NDSP_FORMAT_STEREO_PCM16);

    // Allocate audio buffer
    // 400ms buffer
	const size_t SAMPLES_PER_BUF = vi->rate * 400 / 1000; // 400ms
    // mono (1) or stereo (2)
    const size_t CHANNELS_PER_SAMPLE = vi->channels;
    // s16 buffer
    const size_t WAVEBUF_SIZE = SAMPLES_PER_BUF * CHANNELS_PER_SAMPLE * sizeof(s16);
    const size_t bufferSize = WAVEBUF_SIZE * ARRAY_SIZE(s_waveBufs);
    s_audioBuffer = (int16_t *)linearAlloc(bufferSize);
    if(!s_audioBuffer) {
        printf("Failed to allocate audio buffer\n");
        return false;
    }

    // Setup waveBufs for NDSP
    memset(&s_waveBufs, 0, sizeof(s_waveBufs));
    int16_t *buffer = s_audioBuffer;

    for(size_t i = 0; i < ARRAY_SIZE(s_waveBufs); ++i) {
        s_waveBufs[i].data_vaddr = buffer;
        s_waveBufs[i].nsamples   = WAVEBUF_SIZE / sizeof(buffer[0]);
        s_waveBufs[i].status     = NDSP_WBUF_DONE;

        buffer += WAVEBUF_SIZE / sizeof(buffer[0]);
    }

    return true;
}

static void drawIntroText(int introSpriteIndex) {
	float size = 0.66f;
	C2D_DrawText(&g_staticText[introSpriteIndex],
		C2D_WithColor | C2D_WordWrap,
		(SCREEN_WIDTH/2) - 100,
		(SCREEN_HEIGHT/2) + 45,
		0.5f,
		size, size,
		C2D_Color32(255, 255, 255, 255),
		250.0f,
		0.2f
	);
}

// Audio de-initialisation code
// Stops playback and frees the primary audio buffer
void audioExit(void) {
    ndspChnReset(0);
    linearFree(s_audioBuffer);
}

// Main audio decoding logic
// This function pulls and decodes audio samples from vorbisFile_ to fill waveBuf_
bool fillBuffer(OggVorbis_File *vorbisFile_, ndspWaveBuf *waveBuf_) {
    #ifdef DEBUG
    // Setup timer for performance stats
    TickCounter timer;
    osTickCounterStart(&timer);
    #endif  // DEBUG

    // Decode (2-byte) samples until our waveBuf is full
    int totalBytes = 0;
    while(totalBytes < waveBuf_->nsamples * sizeof(s16)) {
        int16_t *buffer = waveBuf_->data_pcm16 + (totalBytes / sizeof(s16));
        const size_t bufferSize = (waveBuf_->nsamples * sizeof(s16) - totalBytes);

        // Decode bufferSize bytes from vorbisFile_ into buffer,
        // storing the number of bytes that were read (or error)
        const int bytesRead = ov_read(vorbisFile_, (char *)buffer, bufferSize, NULL);
        if(bytesRead <= 0) {
            if(bytesRead == 0) break;  // No error here

            printf("ov_read: error %d (%s)", bytesRead,
                   vorbisStrError(bytesRead));
            break;
        }
        
        totalBytes += bytesRead;
    }

    // If no samples were read in the last decode cycle, we're done
    if(totalBytes == 0) {
        printf("Playback complete, press Start to exit\n");
        return false;
    }

    // Pass samples to NDSP
    // this calculation will make a number <= the previous nsamples
    // = for most cases
    // < for the last possible chunk of the file, which may have less samples before EOF
    // after which we don't care to recover the length
    waveBuf_->nsamples = totalBytes / sizeof(s16);
    ndspChnWaveBufAdd(0, waveBuf_);
    DSP_FlushDataCache(waveBuf_->data_pcm16, totalBytes);

    #ifdef DEBUG
    // Print timing info
    osTickCounterUpdate(&timer);
    printf("fillBuffer %lfms in %lfms\n", totalSamples * 1000.0 / SAMPLE_RATE,
           osTickCounterRead(&timer));
    #endif  // DEBUG

    return true;
}

// NDSP audio frame callback
// This signals the audioThread to decode more things
// once NDSP has played a sound frame, meaning that there should be
// one or more available waveBufs to fill with more data.
void audioCallback(void *const nul_) {
    (void)nul_;  // Unused

    if(s_quit) { // Quit flag
        return;
    }
    
    LightEvent_Signal(&s_event);
}

// Audio thread
// This handles calling the decoder function to fill NDSP buffers as necessary
void audioThread(void *const vorbisFile_) {
    OggVorbis_File *const vorbisFile = (OggVorbis_File *)vorbisFile_;

    while(!s_quit) {  // Whilst the quit flag is unset,
                      // search our waveBufs and fill any that aren't currently
                      // queued for playback (i.e, those that are 'done')
        for(size_t i = 0; i < ARRAY_SIZE(s_waveBufs); ++i) {
            if(s_waveBufs[i].status != NDSP_WBUF_DONE) {
                continue;
            }
            
            if(!fillBuffer(vorbisFile, &s_waveBufs[i])) {   // Playback complete
                return;
            }
        }

        // Wait for a signal that we're needed again before continuing,
        // so that we can yield to other things that want to run
        // (Note that the 3DS uses cooperative threading)
        LightEvent_Wait(&s_event);
    }
}

int main(int argc, char* argv[])
{
	// Init libs
	romfsInit();
	cfguInit();
	ndspInit();
	gfxInitDefault();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
	C2D_Prepare();
	consoleInit(GFX_BOTTOM, NULL);

	// Setup LightEvent for synchronisation of audioThread
    LightEvent_Init(&s_event, RESET_ONESHOT);

    // Open the Ogg Vorbis audio file
    OggVorbis_File vorbisFile;
    FILE *fh = fopen(PATH, "rb");
    int error = ov_open(fh, &vorbisFile, NULL, 0);
    if(error) {
        printf("Failed to open file: error %d (%s)\n", error,
               vorbisStrError(error));
        // Only fclose manually if ov_open failed.
        // If ov_open succeeds, fclose happens in ov_clear.
        fclose(fh);
    }

    // Attempt audioInit
    if(!audioInit(&vorbisFile)) {
        printf("Failed to initialise audio\n");
        ov_clear(&vorbisFile);

        gfxExit();
        ndspExit();
        romfsExit();
        return EXIT_FAILURE;
    }

    // Set the ndsp sound frame callback which signals our audioThread
    ndspSetCallback(audioCallback, NULL);

    // Spawn audio thread

    // Set the thread priority to the main thread's priority ...
    int32_t priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    // ... then subtract 1, as lower number => higher actual priority ...
    priority -= 2;
    // ... finally, clamp it between 0x18 and 0x3F to guarantee that it's valid.
    priority = priority < 0x18 ? 0x18 : priority;
    priority = priority > 0x3F ? 0x3F : priority;

    for (size_t i = 0; i < ARRAY_SIZE(s_waveBufs); i++) {
	    if (!fillBuffer(&vorbisFile, &s_waveBufs[i])) break;
	}

    // Start the thread, passing the address of our vorbisFile as an argument.
    const Thread threadId = threadCreate(audioThread, &vorbisFile,
                                         THREAD_STACK_SZ, priority,
                                         THREAD_AFFINITY, false);
    printf("Created audio thread %p\n", threadId);

	// Create screens
	C3D_RenderTarget* top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);

	// Load graphics
	spriteSheet = C2D_SpriteSheetLoad("romfs:/gfx/intro.t3x");
	if (!spriteSheet) svcBreak(USERBREAK_PANIC);

	// Initialize sprites
	initSprites();
	initText();

	//printf("\x1b[21;16HPress Start to exit.");

	//Print current time
	time_t unixTime = time(NULL);
	size_t introSpriteIndex = 0;
	//static int frame = 0;

	// Main loop
	while (aptMainLoop())
	{
		//gfxSwapBuffers();
		hidScanInput();

		// Your code goes here
		u32 kDown = hidKeysDown();
		if (kDown & KEY_START)
			break; // break in order to return to hbmenu

		// Render the scene
		if (difftime(time(NULL), unixTime) >= 3.0) {
			introSpriteIndex++;
			introSpriteIndex = introSpriteIndex % 11;
			unixTime = time(NULL);
		}

		//if (frame++ % 30 == 0)
		//	printf("\x1b[1;1HScene: %u\x1b[K", introSpriteIndex);

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C2D_TargetClear(top, C2D_Color32f(0.0f, 0.0f, 0.0f, 1.0f));
		C2D_SceneBegin(top);
		C2D_DrawSprite(&sprites[introSpriteIndex].spr);
		drawIntroText(introSpriteIndex);
		C3D_FrameEnd(0);
	}

	// Delete graphics
	C2D_SpriteSheetFree(spriteSheet);
	C2D_TextBufDelete(g_staticBuf);
	C2D_FontFree(font[0]);
	C2D_FontFree(font[1]);
	s_quit = true;
	LightEvent_Signal(&s_event);

    // Free the audio thread
    threadJoin(threadId, UINT64_MAX);
    threadFree(threadId);

    // Cleanup audio things and de-init platform features
    audioExit();
    ndspExit();
    ov_clear(&vorbisFile);

	// Deinit libs
	C2D_Fini();
	C3D_Fini();
	gfxExit();
	romfsExit();
	return 0;
}
